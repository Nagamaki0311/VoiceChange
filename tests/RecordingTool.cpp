#include "RecordingTool.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "core/Engine.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numeric>

// ===== SECTION: RecordingTool実装（T-014） =====
// 検索用アンカー: WAV入出力 / 区間 / 位置合わせ / 指標 / 処理 / レポート / コマンド

namespace vc::rectool
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinDb = -200.0;
constexpr double kFrameSec = 0.01;   // 指標・区間判定のフレーム（10ms）
constexpr int kSpectrumFft = 8192;    // 1/3オクターブの分析窓（48kHzで5.9Hz刻み、170ms）
constexpr double kAlignConfidence = 0.3;

double toDb (double meanSquare) noexcept
{
    return meanSquare > 1.0e-20 ? 10.0 * std::log10 (meanSquare) : kMinDb;
}

double peakToDb (double peak) noexcept
{
    return peak > 1.0e-10 ? 20.0 * std::log10 (peak) : kMinDb;
}

int frameLength (double fs) noexcept
{
    return std::max (1, (int) std::lround (fs * kFrameSec));
}

// 逐次のradix-2 FFT（in-place）。相互相関とスペクトルの分析用（サイズは2の累乗）。
void fftInPlace (std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();

    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;

        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;

        j ^= bit;

        if (i < j)
            std::swap (a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / (double) len;
        const std::complex<double> wl (std::cos (angle), std::sin (angle));

        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);

            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }

    if (inverse)
        for (auto& v : a)
            v /= (double) n;
}

// 範囲 [a, b) のサンプル範囲（この音声の長さに収める）。空ならfalse。
bool sampleRange (const Segment& s, double fs, size_t total, size_t& a, size_t& b) noexcept
{
    const double lo = std::max (0.0, std::round (s.startSec * fs));
    const double hi = std::min ((double) total, std::round (s.endSec * fs));

    if (hi <= lo)
        return false;

    a = (size_t) lo;
    b = (size_t) hi;
    return true;
}
} // namespace

// ===== SECTION: WAV入出力 =====

bool readWav (const juce::File& file, Audio& out, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = "file not found: " + file.getFullPathName();
        return false;
    }

    auto stream = file.createInputStream();

    if (stream == nullptr)
    {
        error = "cannot open file: " + file.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (stream.release(), true));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        error = "cannot read as WAV: " + file.getFullPathName();
        return false;
    }

    const auto length = (int) reader->lengthInSamples;
    const int channels = (int) reader->numChannels;
    juce::AudioBuffer<float> buffer (channels, length);

    if (! reader->read (&buffer, 0, length, 0, true, true))
    {
        error = "read failed: " + file.getFullPathName();
        return false;
    }

    out.sampleRate = reader->sampleRate;
    out.numChannels = channels;
    out.samples.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
    return true;
}

bool writeWav (const juce::File& file, const std::vector<float>& samples, double sampleRate, bool floatFormat, juce::String& error)
{
    auto stream = file.createOutputStream();

    if (stream == nullptr)
    {
        error = "cannot open for writing: " + file.getFullPathName();
        return false;
    }

    // createOutputStreamは既存ファイルの末尾に追記するため、先頭へ戻して切り詰める。
    if (auto* fos = dynamic_cast<juce::FileOutputStream*> (stream.get()))
    {
        fos->setPosition (0);
        fos->truncate();
    }

    std::unique_ptr<juce::OutputStream> os (std::move (stream));
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions {}
                                         .withSampleRate (sampleRate)
                                         .withNumChannels (1)
                                         .withBitsPerSample (floatFormat ? 32 : 16)
                                         .withSampleFormat (floatFormat ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                        : juce::AudioFormatWriterOptions::SampleFormat::integral));

    if (writer == nullptr)
    {
        error = "cannot create WAV writer: " + file.getFullPathName();
        return false;
    }

    const float* channel = samples.data();

    if (! writer->writeFromFloatArrays (&channel, 1, (int) samples.size()))
    {
        error = "write failed: " + file.getFullPathName();
        return false;
    }

    return true; // writerのデストラクタでヘッダーが確定する
}

// ===== SECTION: 区間 =====

const char* segKindId (SegKind kind) noexcept
{
    switch (kind)
    {
        case SegKind::Silence:      return "silence";
        case SegKind::Speech:       return "speech";
        case SegKind::Impact:       return "impact";
        case SegKind::SpeechImpact: return "speech+impact";
    }

    return "silence";
}

bool parseSegments (const juce::String& text, std::vector<Segment>& out, juce::String& error)
{
    out.clear();
    int lineNo = 0;

    for (auto line : juce::StringArray::fromLines (text))
    {
        ++lineNo;
        const auto hash = line.indexOfChar ('#');

        if (hash >= 0)
            line = line.substring (0, hash);

        line = line.trim();

        if (line.isEmpty())
            continue;

        juce::StringArray tokens;
        tokens.addTokens (line, " \t,", "");
        tokens.removeEmptyStrings();

        if (tokens.size() != 3)
        {
            error = "segments line " + juce::String (lineNo) + ": expected \"<start sec> <end sec> <kind>\"";
            return false;
        }

        Segment s;
        s.startSec = tokens[0].getDoubleValue();
        s.endSec = tokens[1].getDoubleValue();
        bool known = false;

        for (auto kind : { SegKind::Silence, SegKind::Speech, SegKind::Impact, SegKind::SpeechImpact })
            if (tokens[2].equalsIgnoreCase (segKindId (kind)))
            {
                s.kind = kind;
                known = true;
            }

        if (! known)
        {
            error = "segments line " + juce::String (lineNo) + ": unknown kind \"" + tokens[2] + "\" (silence, speech, impact, speech+impact)";
            return false;
        }

        if (! (s.startSec >= 0.0 && s.endSec > s.startSec))
        {
            error = "segments line " + juce::String (lineNo) + ": need 0 <= start < end";
            return false;
        }

        out.push_back (s);
    }

    if (out.empty())
    {
        error = "segments file has no segments";
        return false;
    }

    return true;
}

namespace
{
// 10msフレームのdBFS（RMS）。
std::vector<double> frameLevelsDb (const std::vector<float>& x, double fs)
{
    const int h = frameLength (fs);
    const size_t n = x.size() / (size_t) h;
    std::vector<double> d (n);

    for (size_t i = 0; i < n; ++i)
    {
        double sum = 0.0;

        for (int k = 0; k < h; ++k)
        {
            const double v = x[i * (size_t) h + (size_t) k];
            sum += v * v;
        }

        d[i] = toDb (sum / h);
    }

    return d;
}

double percentile (std::vector<double> v, double p)
{
    if (v.empty())
        return kMinDb;

    std::sort (v.begin(), v.end());
    return v[(size_t) std::floor (p * (double) (v.size() - 1))];
}
} // namespace

std::vector<Segment> detectSegments (const std::vector<float>& x, double fs)
{
    const auto d = frameLevelsDb (x, fs);
    const int n = (int) d.size();
    std::vector<Segment> segs;

    if (n == 0)
        return segs;

    const double frameSec = (double) frameLength (fs) / fs;
    const double floorDb = std::max (percentile (d, 0.2), -75.0);
    std::vector<char> nonSilent ((size_t) n, 0);

    // 発話フレームの群（隙間200ms未満を結合）。120ms以上なら発話、未満なら打鍵。
    int groupStart = -1, groupEnd = -1;

    auto flush = [&]
    {
        if (groupStart < 0)
            return;

        const bool speech = groupEnd - groupStart + 1 >= 12;
        const int s = speech ? groupStart : std::max (groupStart - 1, 0);
        const int e = speech ? groupEnd + 1 : std::min (groupEnd + 3, n);
        segs.push_back ({ s * frameSec, e * frameSec, speech ? SegKind::Speech : SegKind::Impact });

        for (int i = std::max (s - 10, 0); i < std::min (e + 10, n); ++i)
            nonSilent[(size_t) i] = 1;

        groupStart = -1;
    };

    for (int i = 0; i < n; ++i)
    {
        if (d[(size_t) i] < floorDb + 20.0)
            continue;

        if (groupStart >= 0 && i - groupEnd > 20)
            flush();

        if (groupStart < 0)
            groupStart = i;

        groupEnd = i;
    }

    flush();

    // 無音: F + 10dB以下で、発話・打鍵から100ms以上離れ、300ms以上続く区間（3フレーム以下の隙間は埋める）。
    std::vector<char> candidate ((size_t) n, 0);

    for (int i = 0; i < n; ++i)
        candidate[(size_t) i] = d[(size_t) i] <= floorDb + 10.0 && ! nonSilent[(size_t) i];

    for (int i = 0; i < n;)
    {
        if (candidate[(size_t) i])
        {
            ++i;
            continue;
        }

        int j = i;

        while (j + 1 < n && ! candidate[(size_t) (j + 1)])
            ++j;

        if (i > 0 && j + 1 < n && j - i + 1 <= 3 && ! std::any_of (nonSilent.begin() + i, nonSilent.begin() + j + 1, [] (char c) { return c != 0; }))
            std::fill (candidate.begin() + i, candidate.begin() + j + 1, 1);

        i = j + 1;
    }

    for (int i = 0; i < n;)
    {
        if (! candidate[(size_t) i])
        {
            ++i;
            continue;
        }

        int j = i;

        while (j + 1 < n && candidate[(size_t) (j + 1)])
            ++j;

        if (j - i + 1 >= 30)
            segs.push_back ({ i * frameSec, (j + 1) * frameSec, SegKind::Silence });

        i = j + 1;
    }

    std::sort (segs.begin(), segs.end(), [] (const Segment& a, const Segment& b) { return a.startSec < b.startSec; });
    return segs;
}

// ===== SECTION: 位置合わせ =====

Alignment measureAlignment (const std::vector<float>& ref, const std::vector<float>& test, double fs, double maxLagSec)
{
    Alignment result;

    if (ref.size() < 64 || test.size() < 64)
        return result;

    // 粗い探索: 4サンプルの平均で間引いてFFTで相互相関を取る。細かい探索: 元のレートで±4サンプル。
    constexpr size_t kDecim = 4;
    auto decimate = [] (const std::vector<float>& v)
    {
        std::vector<double> out (v.size() / kDecim);

        for (size_t i = 0; i < out.size(); ++i)
        {
            double s = 0.0;

            for (size_t k = 0; k < kDecim; ++k)
                s += v[i * kDecim + k];

            out[i] = s / (double) kDecim;
        }

        return out;
    };

    const auto a = decimate (ref);
    const auto b = decimate (test);
    size_t size = 1;

    while (size < a.size() + b.size())
        size <<= 1;

    std::vector<std::complex<double>> fa (size), fb (size);

    for (size_t i = 0; i < a.size(); ++i)
        fa[i] = a[i];

    for (size_t i = 0; i < b.size(); ++i)
        fb[i] = b[i];

    fftInPlace (fa, false);
    fftInPlace (fb, false);

    for (size_t i = 0; i < size; ++i)
        fa[i] = std::conj (fa[i]) * fb[i]; // c[m] = sum_n a[n] b[n + m]

    fftInPlace (fa, true);

    const long maxLag = (long) std::floor (maxLagSec * fs / (double) kDecim);
    long bestLag = 0;
    double best = -1.0e300;

    for (long m = -maxLag; m <= maxLag; ++m)
    {
        const size_t index = (size_t) ((m + (long) size) % (long) size);

        if (fa[index].real() > best)
        {
            best = fa[index].real();
            bestLag = m;
        }
    }

    // 元のレートで精密化し、正規化する。
    auto correlationAt = [&] (long lag)
    {
        double sum = 0.0;

        for (size_t n = 0; n < ref.size(); ++n)
        {
            const long m = (long) n + lag;

            if (m >= 0 && m < (long) test.size())
                sum += (double) ref[n] * test[(size_t) m];
        }

        return sum;
    };

    long fineLag = bestLag * (long) kDecim;
    double fineBest = -1.0e300;

    for (long lag = bestLag * (long) kDecim - 4; lag <= bestLag * (long) kDecim + 4; ++lag)
    {
        const double c = correlationAt (lag);

        if (c > fineBest)
        {
            fineBest = c;
            fineLag = lag;
        }
    }

    double energyRef = 0.0, energyTest = 0.0;

    for (auto v : ref)
        energyRef += (double) v * v;

    for (auto v : test)
        energyTest += (double) v * v;

    result.lagSamples = (int) fineLag;
    result.correlation = energyRef > 0.0 && energyTest > 0.0 ? std::max (0.0, fineBest / std::sqrt (energyRef * energyTest)) : 0.0;
    result.confident = result.correlation >= kAlignConfidence;
    return result;
}

namespace
{
// BS.1770のK特性を通した信号（double）。プレフィルタ（高域シェルフ）→RLB（高域通過）の2つのバイクワッド。
std::vector<double> kWeight (const std::vector<float>& x, double fs)
{
    const double f0a = 1681.974450955533, Ga = 3.999843853973347, Qa = 0.7071752369554196;
    const double ka = std::tan (kPi * f0a / fs), vh = std::pow (10.0, Ga / 20.0), vb = std::pow (vh, 0.4996667741545416);
    const double a0a = 1.0 + ka / Qa + ka * ka;
    const double b1[3] = { (vh + vb * ka / Qa + ka * ka) / a0a, 2.0 * (ka * ka - vh) / a0a, (vh - vb * ka / Qa + ka * ka) / a0a };
    const double a1[2] = { 2.0 * (ka * ka - 1.0) / a0a, (1.0 - ka / Qa + ka * ka) / a0a };

    const double f0b = 38.13547087602444, Qb = 0.5003270373238773;
    const double kb = std::tan (kPi * f0b / fs);
    const double a0b = 1.0 + kb / Qb + kb * kb;
    const double a2[2] = { 2.0 * (kb * kb - 1.0) / a0b, (1.0 - kb / Qb + kb * kb) / a0b };

    std::vector<double> y (x.size());
    double s1a = 0.0, s2a = 0.0, s1b = 0.0, s2b = 0.0; // 転置直接形II
    const double b2[3] = { 1.0, -2.0, 1.0 };

    for (size_t i = 0; i < x.size(); ++i)
    {
        const double in = x[i];
        const double v = b1[0] * in + s1a;
        s1a = b1[1] * in - a1[0] * v + s2a;
        s2a = b1[2] * in - a1[1] * v;

        const double w = b2[0] * v + s1b;
        s1b = b2[1] * v - a2[0] * w + s2b;
        s2b = b2[2] * v - a2[1] * w;
        y[i] = w;
    }

    return y;
}
} // namespace

double kWeightedLevelDb (const std::vector<float>& x, double fs)
{
    const auto y = kWeight (x, fs);
    double sum = 0.0;

    for (auto v : y)
        sum += v * v;

    return y.empty() ? kMinDb : toDb (sum / (double) y.size());
}

// ===== SECTION: 指標 =====

namespace
{
// 範囲群の長時間平均パワースペクトルを1/3オクターブごとに積分した値 [dBFS]（正弦波の振幅Aが A^2/2 のパワー。RMSのdBFSと同じ基準）。
// Hann窓・50%重なり。窓（kSpectrumFft）より短い範囲は使わない。1フレームもなければfalse。
bool bandLevels (const std::vector<float>& x, double fs, const std::vector<std::pair<size_t, size_t>>& ranges, BandDb& out)
{
    const size_t n = (size_t) kSpectrumFft;
    std::vector<double> window (n);
    double windowPower = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        window[i] = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n);
        windowPower += window[i] * window[i];
    }

    std::vector<double> psd (n / 2 + 1, 0.0);
    std::vector<std::complex<double>> buf (n);
    int frames = 0;

    for (const auto& r : ranges)
        for (size_t pos = r.first; pos + n <= r.second; pos += n / 2)
        {
            for (size_t i = 0; i < n; ++i)
                buf[i] = (double) x[pos + i] * window[i];

            fftInPlace (buf, false);

            for (size_t k = 0; k <= n / 2; ++k)
            {
                const double scale = (k == 0 || k == n / 2) ? 1.0 : 2.0; // 片側
                psd[k] += scale * std::norm (buf[k]) / (fs * windowPower);
            }

            ++frames;
        }

    if (frames == 0)
    {
        out.fill (kMinDb);
        return false;
    }

    const double df = fs / (double) n;

    for (int b = 0; b < kNumBands; ++b)
    {
        const double centre = 1000.0 * std::pow (2.0, (double) (b - 12) / 3.0);
        const double lo = centre / std::pow (2.0, 1.0 / 6.0);
        const double hi = centre * std::pow (2.0, 1.0 / 6.0);
        double power = 0.0;

        for (size_t k = 0; k <= n / 2; ++k)
        {
            const double binLo = ((double) k - 0.5) * df, binHi = ((double) k + 0.5) * df;
            const double overlap = std::min (binHi, hi) - std::max (binLo, lo);

            if (overlap > 0.0)
                power += psd[k] / (double) frames * overlap;
        }

        out[(size_t) b] = toDb (power);
    }

    return true;
}

FileStats measureFile (const juce::String& name, const std::vector<float>& x, double fs, const std::vector<Segment>& segmentsRawTime,
                       const Alignment& alignment, int lag, bool shares)
{
    FileStats st;
    st.name = name;
    st.alignment = alignment;
    st.sharesSegments = shares;
    st.silenceBands.fill (kMinDb);
    st.speechBands.fill (kMinDb);

    const auto kFiltered = kWeight (x, fs);
    double speechKSum = 0.0;
    double silenceSum = 0.0, speechSum = 0.0;
    size_t silenceCount = 0, speechCount = 0;
    std::vector<double> silenceFrames;
    std::vector<std::pair<size_t, size_t>> silenceRanges, speechRanges;
    const int h = frameLength (fs);

    for (auto s : segmentsRawTime)
    {
        s.startSec += (double) lag / fs;
        s.endSec += (double) lag / fs;
        st.segments.push_back (s);

        SegmentStat stat;
        size_t a = 0, b = 0;

        if (sampleRange (s, fs, x.size(), a, b))
        {
            double sum = 0.0, peak = 0.0;

            for (size_t i = a; i < b; ++i)
            {
                sum += (double) x[i] * x[i];
                peak = std::max (peak, (double) std::abs (x[i]));
            }

            stat.rmsDb = toDb (sum / (double) (b - a));
            stat.peakDb = peakToDb (peak);

            if (s.kind == SegKind::Silence)
            {
                silenceSum += sum;
                silenceCount += b - a;
                silenceRanges.emplace_back (a, b);

                for (size_t f = a; f + (size_t) h <= b; f += (size_t) h)
                {
                    double fsum = 0.0;

                    for (int k = 0; k < h; ++k)
                        fsum += (double) x[f + (size_t) k] * x[f + (size_t) k];

                    silenceFrames.push_back (toDb (fsum / h));
                }
            }
            else if (s.kind == SegKind::Speech)
            {
                speechSum += sum;
                speechCount += b - a;

                for (size_t i = a; i < b; ++i)
                    speechKSum += kFiltered[i] * kFiltered[i];

                speechRanges.emplace_back (a, b);
            }
        }

        st.segmentStats.push_back (stat);
    }

    if (silenceCount > 0)
    {
        st.silenceSec = (double) silenceCount / fs;
        st.silenceRmsDb = toDb (silenceSum / (double) silenceCount);
        st.silenceMedianFrameDb = percentile (silenceFrames, 0.5);
        st.silenceDigitalFraction = silenceFrames.empty() ? 0.0
            : (double) std::count_if (silenceFrames.begin(), silenceFrames.end(), [] (double v) { return v < kDigitalSilenceDb; }) / (double) silenceFrames.size();
        st.silenceBandsValid = bandLevels (x, fs, silenceRanges, st.silenceBands);
    }

    if (speechCount > 0)
    {
        st.speechSec = (double) speechCount / fs;
        st.speechRmsDb = toDb (speechSum / (double) speechCount);
        st.speechKDb = toDb (speechKSum / (double) speechCount);
        st.speechBandsValid = bandLevels (x, fs, speechRanges, st.speechBands);
    }

    return st;
}
} // namespace

CompareResult compareRecordings (const std::vector<float>& raw, const std::vector<float>& sonar, const std::vector<float>& ours,
                                 double fs, const std::vector<Segment>* segments, const std::vector<Segment>* sonarSegments)
{
    CompareResult result;
    result.sampleRate = fs;
    result.segmentsFromFile = segments != nullptr;
    result.sonarSegmentsFromFile = sonarSegments != nullptr;

    const auto rawSegments = segments != nullptr ? *segments : detectSegments (raw, fs);

    const auto oursAlign = measureAlignment (raw, ours, fs);
    const auto sonarAlign = measureAlignment (raw, sonar, fs);
    const int oursLag = oursAlign.confident ? oursAlign.lagSamples : 0; // oursはrawから作った音声（信頼できなければ同じ時間軸とみなす）
    const bool sonarShares = sonarSegments == nullptr && (segments != nullptr || sonarAlign.confident);
    const int sonarLag = sonarAlign.confident ? sonarAlign.lagSamples : 0;
    const auto sonarOwn = sonarSegments != nullptr ? *sonarSegments : detectSegments (sonar, fs);

    result.files[0] = measureFile ("raw", raw, fs, rawSegments, {}, 0, true);
    result.files[1] = measureFile ("sonar", sonar, fs, sonarShares ? rawSegments : sonarOwn, sonarAlign, sonarLag, sonarShares);
    result.files[2] = measureFile ("ours", ours, fs, rawSegments, oursAlign, oursLag, true);
    return result;
}

// ===== SECTION: レポート =====

namespace
{
juce::String num (double v, int width, int decimals = 1)
{
    const auto text = decimals > 0 ? juce::String (v, decimals) : juce::String ((int) std::lround (v)); // juce::String(double, 0)は桁数指定なし扱いのため
    return (v <= kMinDb + 1.0 ? juce::String ("---") : text).paddedLeft (' ', width);
}

juce::String signedNum (double v, int width)
{
    return (v > 0.0 ? "+" + juce::String (v, 1) : juce::String (v, 1)).paddedLeft (' ', width);
}

juce::String levelWithFlag (double db, int width)
{
    return (num (db, 0) + (db < kDigitalSilenceDb && db > kMinDb + 1.0 ? "*" : "")).paddedLeft (' ', width);
}

juce::String timeRange (const Segment& s)
{
    return (juce::String (s.startSec, 2) + "-" + juce::String (s.endSec, 2)).paddedRight (' ', 12);
}

// 区間の表。columns: ours/rawで同じ区間を並べる（sonarが別区間なら別表）。
void appendSegmentTable (juce::String& out, const juce::String& title, const std::vector<const FileStats*>& files)
{
    const auto& ref = *files.front();
    out << title << "\n";
    out << " #  区間(秒)      種類            ";

    for (auto* f : files)
        out << ("RMS " + f->name).paddedLeft (' ', 11) << ("peak " + f->name).paddedLeft (' ', 11);

    out << "\n";

    for (size_t i = 0; i < ref.segments.size(); ++i)
    {
        out << juce::String ((int) i + 1).paddedLeft (' ', 2) << "  " << timeRange (ref.segments[i]) << " "
            << juce::String (segKindId (ref.segments[i].kind)).paddedRight (' ', 15);

        for (auto* f : files)
            out << num (f->segmentStats[i].rmsDb, 11) << num (f->segmentStats[i].peakDb, 11);

        // 打鍵の区間は、rawに対する最後のファイルのピーク差（本アプリの処理による減衰量）を添える。
        if ((ref.segments[i].kind == SegKind::Impact || ref.segments[i].kind == SegKind::SpeechImpact) && files.size() >= 2)
            out << "   peak差(" << files.back()->name << "-" << ref.name << ") "
                << signedNum (files.back()->segmentStats[i].peakDb - ref.segmentStats[i].peakDb, 6) << " dB";

        out << "\n";
    }

    out << "\n";
}
} // namespace

juce::String formatReport (const CompareResult& r)
{
    juce::String out;
    const auto& raw = r.files[0];
    const auto& sonar = r.files[1];
    const auto& ours = r.files[2];

    out << juce::String::fromUTF8 ("# 実録音の比較（サンプルレート ") << juce::String (r.sampleRate, 0) << " Hz）\n";
    out << juce::String::fromUTF8 ("区間: ") << (r.segmentsFromFile ? juce::String::fromUTF8 ("区間ファイル") : juce::String::fromUTF8 ("生音声のエネルギーで自動判定"))
        << "\n\n";

    out << juce::String::fromUTF8 ("## 位置合わせ（相互相関、rawに対する遅れ。0.3以上で同じ信号とみなす）\n");

    for (auto* f : { &sonar, &ours })
    {
        const bool isOurs = f == &ours;
        out << "  " << f->name.paddedRight (' ', 6) << " lag " << juce::String (f->alignment.lagSamples).paddedLeft (' ', 6) << " samples ("
            << juce::String (1000.0 * f->alignment.lagSamples / r.sampleRate, 2) << " ms)  correlation " << juce::String (f->alignment.correlation, 3)
            << (f->alignment.confident ? juce::String::fromUTF8 ("  → 補正して比較")
                : isOurs ? juce::String::fromUTF8 ("  → 相関が低い（ノイズ除去が強く効いた無発話などでは正常）。rawから作った音声なのでlag 0（処理側で遅延補正済み）として扱う")
                         : juce::String::fromUTF8 ("  → 同じ信号ではない（別テイク）。補正せず、lag 0として扱う"))
            << "\n";
    }

    if (! sonar.sharesSegments)
        out << juce::String::fromUTF8 (r.sonarSegmentsFromFile ? "  sonarは別テイクとして、sonar用の区間ファイルの区間（sonar自身の時間軸）を使った。\n"
                                                              : "  sonarは別テイクとみなし、sonar自身のエネルギーで区間を自動判定した（時刻はraw・oursと対応しない）。\n");

    out << "\n";

    if (sonar.sharesSegments)
        appendSegmentTable (out, juce::String::fromUTF8 ("## 区間ごとのレベル [dBFS]"), { &raw, &sonar, &ours });
    else
    {
        appendSegmentTable (out, juce::String::fromUTF8 ("## 区間ごとのレベル [dBFS]（raw・ours。rawの区間）"), { &raw, &ours });
        appendSegmentTable (out, juce::String::fromUTF8 ("## 区間ごとのレベル [dBFS]（sonar。sonar自身の区間）"), { &sonar });
    }

    out << juce::String::fromUTF8 ("## 集計\n");
    out << "                                  " << raw.name.paddedLeft (' ', 9) << sonar.name.paddedLeft (' ', 9) << ours.name.paddedLeft (' ', 9) << "\n";

    out << juce::String::fromUTF8 ("  無声区間の長さ [s]              ");

    for (auto* f : { &raw, &sonar, &ours })
        out << num (f->silenceSec, 9, 2);

    out << juce::String::fromUTF8 ("\n  無声区間の残留雑音 RMS [dBFS]   ");

    for (auto* f : { &raw, &sonar, &ours })
        out << (f->silenceSec > 0.0 ? levelWithFlag (f->silenceRmsDb, 9) : juce::String ("n/a").paddedLeft (' ', 9));

    out << juce::String::fromUTF8 ("\n  同（10msフレームの中央値）      ");

    for (auto* f : { &raw, &sonar, &ours })
        out << (f->silenceSec > 0.0 ? levelWithFlag (f->silenceMedianFrameDb, 9) : juce::String ("n/a").paddedLeft (' ', 9));

    out << juce::String::fromUTF8 ("\n  同（-100dBFS未満のフレーム）[%] ");

    for (auto* f : { &raw, &sonar, &ours })
        out << (f->silenceSec > 0.0 ? num (100.0 * f->silenceDigitalFraction, 9, 0) : juce::String ("n/a").paddedLeft (' ', 9));

    out << juce::String::fromUTF8 ("\n  発話区間の長さ [s]              ");

    for (auto* f : { &raw, &sonar, &ours })
        out << num (f->speechSec, 9, 2);

    out << juce::String::fromUTF8 ("\n  発話区間のレベル RMS [dBFS]     ");

    for (auto* f : { &raw, &sonar, &ours })
        out << (f->speechSec > 0.0 ? num (f->speechRmsDb, 9) : juce::String ("n/a").paddedLeft (' ', 9));

    out << juce::String::fromUTF8 ("\n  発話と残留雑音の比 [dB]         ");

    for (auto* f : { &raw, &sonar, &ours })
    {
        if (f->speechSec > 0.0 && f->silenceSec > 0.0)
            out << (num (f->snrDb(), 8) + (f->silenceRmsDb < kDigitalSilenceDb ? ">" : " "));
        else
            out << juce::String ("n/a").paddedLeft (' ', 9);
    }

    out << "\n";

    // 発話区間のラウドネス（声量）。生に対する差が主指標。K特性はBS.1770の重み付け（ゲートなしの簡易版、絶対値はLUFSではない）。
    out << juce::String::fromUTF8 ("  発話区間のK特性重み付けパワー [dB]");

    for (auto* f : { &raw, &sonar, &ours })
        out << (f->speechSec > 0.0 ? num (f->speechKDb, 9) : juce::String ("n/a").paddedLeft (' ', 9));

    out << "\n";

    if (raw.speechSec > 0.0)
    {
        out << juce::String::fromUTF8 ("  生に対する差 [dB]  RMS (非加重)     ") << juce::String ("-").paddedLeft (' ', 7);

        for (auto* f : { &sonar, &ours })
            out << (f->speechSec > 0.0 ? signedNum (f->speechRmsDb - raw.speechRmsDb, 9) : juce::String ("n/a").paddedLeft (' ', 9));

        out << juce::String::fromUTF8 ("\n  生に対する差 [dB]  K特性重み付け    ") << juce::String ("-").paddedLeft (' ', 7);

        for (auto* f : { &sonar, &ours })
            out << (f->speechSec > 0.0 ? signedNum (f->speechKDb - raw.speechKDb, 9) : juce::String ("n/a").paddedLeft (' ', 9));

        out << "\n";

        if (ours.speechSec > 0.0)
        {
            const double d = ours.speechKDb - raw.speechKDb;
            out << juce::String::fromUTF8 ("  声量の判定（暫定基準: oursのK特性の差が -0.5dB 以上）: ") << (d >= -0.5 ? juce::String::fromUTF8 ("合格") : juce::String::fromUTF8 ("下回る（発話を小さくしている）")) << "\n";
        }
    }

    out << juce::String::fromUTF8 ("  * = デジタル無音（-100dBFS未満）。圧縮（AAC等）の影響で、この付近以下の比較は不正確。> = 下限（雑音がデジタル無音のため比は実際より小さい値）\n");

    if (raw.silenceSec < 1.0 || raw.speechSec < 1.0 || ours.silenceSec < 1.0 || ours.speechSec < 1.0)
        out << juce::String::fromUTF8 ("  注意: 無声区間または発話区間の合計が1秒未満のファイルがある。その値は参考にとどめる。\n");

    out << "\n";

    // 無声区間のスペクトル。
    out << juce::String::fromUTF8 ("## 無声区間の1/3オクターブスペクトル [dBFS]（帯域内のパワー）\n");
    out << juce::String::fromUTF8 ("  Hz        ") << raw.name.paddedLeft (' ', 9) << sonar.name.paddedLeft (' ', 9) << ours.name.paddedLeft (' ', 9) << "\n";

    for (int b = 0; b < kNumBands; ++b)
    {
        out << "  " << juce::String (kBandNominalHz[(size_t) b]).paddedRight (' ', 8);

        for (auto* f : { &raw, &sonar, &ours })
            out << (f->silenceBandsValid ? num (f->silenceBands[(size_t) b], 9) : juce::String ("n/a").paddedLeft (' ', 9));

        out << "\n";
    }

    out << "\n";

    // 発話区間の長時間平均スペクトルとの差。
    out << juce::String::fromUTF8 ("## 発話区間の長時間平均スペクトル（1/3オクターブ）。差の列は dB\n");
    out << juce::String::fromUTF8 ("  「差」は絶対値（レベル差を含む）、「形」は各列の平均（63Hz〜8kHz）を引いた値（EQの形だけを見る）。sonarが別テイクなら、内容の違いも含む統計的な比較。\n");
    out << juce::String::fromUTF8 ("  Hz        ") << raw.name.paddedLeft (' ', 8) << sonar.name.paddedLeft (' ', 8) << ours.name.paddedLeft (' ', 8)
        << juce::String::fromUTF8 ("  差 s-r   差 o-r   差 o-s    形 s-r   形 o-r   形 o-s") << "\n";

    const bool haveSpectra = raw.speechBandsValid && sonar.speechBandsValid && ours.speechBandsValid;

    if (! haveSpectra)
        out << juce::String::fromUTF8 ("  （発話区間がないか短い（170ms未満）ファイルがあるため表を出せない）\n");
    else
    {
        BandDb sr {}, orr {}, os {};
        double meanSr = 0.0, meanOr = 0.0, meanOs = 0.0;

        for (int b = 0; b < kNumBands; ++b)
        {
            const auto i = (size_t) b;
            sr[i] = sonar.speechBands[i] - raw.speechBands[i];
            orr[i] = ours.speechBands[i] - raw.speechBands[i];
            os[i] = ours.speechBands[i] - sonar.speechBands[i];
            meanSr += sr[i] / kNumBands;
            meanOr += orr[i] / kNumBands;
            meanOs += os[i] / kNumBands;
        }

        for (int b = 0; b < kNumBands; ++b)
        {
            const auto i = (size_t) b;
            out << "  " << juce::String (kBandNominalHz[i]).paddedRight (' ', 8) << num (raw.speechBands[i], 8) << num (sonar.speechBands[i], 8)
                << num (ours.speechBands[i], 8) << "  " << signedNum (sr[i], 7) << " " << signedNum (orr[i], 7) << " " << signedNum (os[i], 7) << "   "
                << signedNum (sr[i] - meanSr, 7) << " " << signedNum (orr[i] - meanOr, 7) << " " << signedNum (os[i] - meanOs, 7) << "\n";
        }

        out << juce::String::fromUTF8 ("  平均（全帯域のレベル差）      ") << "                          " << signedNum (meanSr, 7) << " " << signedNum (meanOr, 7) << " "
            << signedNum (meanOs, 7) << "\n";
    }

    return out;
}

// ===== SECTION: 処理 =====

void applyEqPreset (ProcessSettings& s, EqPreset preset)
{
    using T = EqType;

    switch (preset)
    {
        case EqPreset::Off:
            s.eqEnabled = false;
            break;

        case EqPreset::On:
            s.eqEnabled = true;
            break;

        case EqPreset::A2: // 自然なクリアさ（Params.hのkEqPresets）
        case EqPreset::A3: // 輪郭はっきり
        {
            const auto& spec = kEqPresets[preset == EqPreset::A2 ? 0 : 1];
            s.eqEnabled = true;
            s.eqBands = spec.bands;
            s.eqOutputGainDb = spec.outputGainDb;
            break;
        }

        case EqPreset::Sonar: // ユーザーの現在のSonarのEQの5バンド近似
            s.eqEnabled = true;
            s.eqBands = { { { true, T::LowShelf, 26.0f, -18.0f, 0.48f },
                            { true, T::Peak, 216.0f, 4.1f, 0.75f },
                            { true, T::Peak, 546.0f, -0.5f, 1.44f },
                            { true, T::Peak, 2062.0f, 4.1f, 0.70f },
                            { true, T::HighShelf, 5741.0f, -2.3f, 1.19f } } };
            s.eqOutputGainDb = 0.0f;
            break;
    }
}

ProcessResult processAudio (const std::vector<float>& input, double fs, const ProcessSettings& s)
{
    ProcessResult result;
    const int block = std::max (1, (int) std::lround (fs * kFrameSec));

    Engine engine;
    auto& p = engine.params();
    p.preset.store ((int) Preset::Normal);
    p.gainDb.store (0.0f);
    p.enabled.store (true);
    p.nrEnabled.store (s.nrEnabled);
    p.nrBackground.store (s.nrBackground);
    p.nrImpact.store (s.nrImpact);
    p.eqEnabled.store (s.eqEnabled);
    p.eqOutputGainDb.store (s.eqOutputGainDb);

    for (size_t i = 0; i < s.eqBands.size(); ++i)
        p.eqBands[i].store (s.eqBands[i]);

    engine.prepare ({ fs, block }); // パラメータを先に設定する（Engine::prepareがゲインの初期値に使うため）

    const int latency = s.nrEnabled ? engine.debugNoiseReducer().getDelaySamples() : 0;
    result.latencySamples = latency;

    // 入力の後ろに遅延ぶんの無音を足して流し、先頭のlatencyサンプルを捨てる（入力と同じ長さ・同じ位置）。
    const size_t total = input.size() + (size_t) latency;
    const size_t padded = (total + (size_t) block - 1) / (size_t) block * (size_t) block;
    std::vector<float> buf (padded, 0.0f);
    std::copy (input.begin(), input.end(), buf.begin());

    int prevClosed = 0, prevImpact = 0;

    for (size_t pos = 0; pos < padded; pos += (size_t) block)
    {
        engine.process (buf.data() + pos, block);

        const int closed = engine.debugNoiseReducer().getGateClosedSampleCount();
        const int impact = engine.debugNoiseReducer().getImpactAttenuatedSampleCount();
        const bool counted = closed >= prevClosed && impact >= prevImpact; // reset（異常時）で累計が戻ったブロックは数えない
        result.blocks.push_back ({ ((double) pos - latency) / fs, block, counted ? closed - prevClosed : 0, counted ? impact - prevImpact : 0 });
        prevClosed = closed;
        prevImpact = impact;
    }

    result.output.assign (buf.begin() + latency, buf.begin() + latency + (long) input.size());
    result.errorFlags = engine.getErrorFlags();
    result.underflowCount = engine.debugNoiseReducer().getUnderflowCount();
    return result;
}

// ===== SECTION: コマンド =====

namespace
{
struct ParsedArgs
{
    juce::StringArray positional;
    juce::StringPairArray options;
    juce::String error;
};

ParsedArgs parseArgs (const juce::StringArray& args, const juce::StringArray& valueOptions, const juce::StringArray& flagOptions = {})
{
    ParsedArgs parsed;

    for (int i = 0; i < args.size(); ++i)
    {
        if (! args[i].startsWith ("--"))
            parsed.positional.add (args[i]);
        else if (flagOptions.contains (args[i]))
            parsed.options.set (args[i], "1");
        else if (valueOptions.contains (args[i]) && i + 1 < args.size())
        {
            parsed.options.set (args[i], args[i + 1]);
            ++i;
        }
        else
        {
            parsed.error = "unknown option or missing value: " + args[i];
            return parsed;
        }
    }

    return parsed;
}

void print (const juce::String& s)
{
    std::fputs (s.toRawUTF8(), stdout);
    std::fflush (stdout);
}

// エラーメッセージ（末尾に改行を付ける）と、必要なら使い方を標準エラーへ出す。
void printError (const juce::String& message, const char* usage = nullptr)
{
    if (message.isNotEmpty())
        std::fprintf (stderr, "%s\n", message.toRawUTF8());

    if (usage != nullptr)
        std::fputs (usage, stderr);
}

// 0〜1の数値だけを受け付ける（数値でない・範囲外はfalse）。
bool parseUnit (const juce::String& text, float& out)
{
    const auto utf8 = text.trim().toStdString();
    char* end = nullptr;
    const double v = std::strtod (utf8.c_str(), &end);

    if (utf8.empty() || end == utf8.c_str() || *end != '\0' || ! (v >= 0.0 && v <= 1.0))
        return false;

    out = (float) v;
    return true;
}

// 区間ファイルの時刻が音声の長さを超えていたら警告する（処理は続ける。台本と別の録音を指定した取り違えの検出用）。
void warnSegmentsBeyond (const std::vector<Segment>& segs, double durationSec, const char* label)
{
    for (const auto& s : segs)
        if (s.endSec > durationSec + 0.01)
        {
            printError (juce::String ("warning: ") + label + " has a segment ending at " + juce::String (s.endSec, 2) + " s, beyond the audio length ("
                        + juce::String (durationSec, 2) + " s). Is it the right file?");
            return;
        }
}

bool loadSegmentsFile (const juce::String& path, std::vector<Segment>& segs, juce::String& error)
{
    const juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (path));

    if (! file.existsAsFile())
    {
        error = "segments file not found: " + file.getFullPathName();
        return false;
    }

    return parseSegments (file.loadFileAsString(), segs, error);
}

double rmsOf (const std::vector<float>& x, long a, long b)
{
    a = std::max (a, 0L);
    b = std::min (b, (long) x.size());

    if (b <= a)
        return kMinDb;

    double sum = 0.0;

    for (long i = a; i < b; ++i)
        sum += (double) x[(size_t) i] * x[(size_t) i];

    return toDb (sum / (double) (b - a));
}
} // namespace

int runProcessWav (const juce::StringArray& args)
{
    const auto parsed = parseArgs (args, { "--bg", "--impact", "--eq", "--eq-gain", "--nr", "--settings", "--segments", "--trace" }, { "--float" });
    const char* usage = "usage: VoiceChangeTests --process-wav <in.wav> <out.wav> [--bg 0.65] [--impact 0.15] [--nr on|off] [--eq on|off|a2|a3|sonar] [--eq-gain -12..12]\n"
                        "                                     [--settings <VoiceChange.settings>] [--segments <file>] [--trace <csv>] [--float]\n";

    if (! parsed.error.isEmpty() || parsed.positional.size() != 2)
    {
        printError (parsed.error, usage);
        return 2;
    }

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto inFile = cwd.getChildFile (parsed.positional[0]);
    const auto outFile = cwd.getChildFile (parsed.positional[1]);
    juce::String error;

    // 入力を上書きしない（writeWavは既存ファイルを上書きする）。シンボリックリンク経由も同一とみなす。
    if (inFile == outFile || inFile.getLinkedTarget() == outFile.getLinkedTarget())
    {
        printError ("input and output are the same file: " + inFile.getFullPathName());
        return 2;
    }

    // 設定: 既定（ノイズ除去ON、背景・インパクトは本アプリの初期値、EQ OFF）→ 設定ファイル → オプション、の順に上書きする。
    ProcessSettings settings;

    if (parsed.options.containsKey ("--settings"))
    {
        const auto settingsFile = cwd.getChildFile (parsed.options["--settings"]);

        if (! settingsFile.existsAsFile())
        {
            printError ("settings file not found: " + settingsFile.getFullPathName());
            return 2;
        }

        // PropertiesFile（juce_data_structures）が書くXML（<PROPERTIES><VALUE name= val=/>）をPropertySetへ読む。
        const auto xml = juce::XmlDocument::parse (settingsFile);

        if (xml == nullptr || ! xml->hasTagName ("PROPERTIES"))
        {
            printError ("cannot parse settings file: " + settingsFile.getFullPathName());
            return 2;
        }

        juce::PropertySet props;
        props.restoreFromXml (*xml);
        const auto saved = loadSettings (props);
        settings.nrEnabled = saved.nrEnabled;
        settings.nrBackground = saved.nrBackground;
        settings.nrImpact = saved.nrImpact;
        settings.eqEnabled = saved.eqEnabled;
        settings.eqBands = saved.eqBands;
        settings.eqOutputGainDb = saved.eqOutputGainDb;
    }

    auto readUnit = [&] (const char* key, float& target)
    {
        if (! parsed.options.containsKey (key) || parseUnit (parsed.options[key], target))
            return true;

        printError (juce::String (key) + " must be a number from 0 to 1: " + parsed.options[key]);
        return false;
    };

    if (! readUnit ("--bg", settings.nrBackground) || ! readUnit ("--impact", settings.nrImpact))
        return 2;

    if (parsed.options.containsKey ("--nr"))
    {
        const auto v = parsed.options["--nr"].toLowerCase();

        if (v != "on" && v != "off")
        {
            printError ("--nr must be on or off");
            return 2;
        }

        settings.nrEnabled = v == "on";
    }

    if (parsed.options.containsKey ("--eq"))
    {
        const auto v = parsed.options["--eq"].toLowerCase();

        if (v == "on")       applyEqPreset (settings, EqPreset::On);
        else if (v == "off") applyEqPreset (settings, EqPreset::Off);
        else if (v == "a2")  applyEqPreset (settings, EqPreset::A2);
        else if (v == "a3")  applyEqPreset (settings, EqPreset::A3);
        else if (v == "sonar") applyEqPreset (settings, EqPreset::Sonar);
        else
        {
            printError ("--eq must be on, off, a2, a3 or sonar");
            return 2;
        }
    }

    // 出力ゲイン（dB）。--eqのプリセットや設定ファイルの値を上書きする（ゲインを振って声量を測るため）。
    if (parsed.options.containsKey ("--eq-gain"))
    {
        const auto utf8 = parsed.options["--eq-gain"].trim().toStdString();
        char* end = nullptr;
        const double v = std::strtod (utf8.c_str(), &end);

        if (utf8.empty() || end == utf8.c_str() || *end != '\0' || ! (v >= -(double) kEqMaxOutputGainDb && v <= (double) kEqMaxOutputGainDb))
        {
            printError ("--eq-gain must be a number from -12 to 12: " + parsed.options["--eq-gain"]);
            return 2;
        }

        settings.eqOutputGainDb = (float) v;
    }

    Audio in;

    if (! readWav (inFile, in, error))
    {
        std::fprintf (stderr, "%s\n", error.toRawUTF8());
        return 2;
    }

    const auto result = processAudio (in.samples, in.sampleRate, settings);

    const bool floatOut = parsed.options.containsKey ("--float");

    if (! writeWav (outFile, result.output, in.sampleRate, floatOut, error))
    {
        std::fprintf (stderr, "%s\n", error.toRawUTF8());
        return 2;
    }

    juce::String out;
    out << "in:  " << inFile.getFullPathName() << " (" << juce::String ((double) in.samples.size() / in.sampleRate, 2) << " s, "
        << juce::String (in.sampleRate, 0) << " Hz" << (in.numChannels > 1 ? ", channel 1 of " + juce::String (in.numChannels) : juce::String()) << ")\n";
    out << "out: " << outFile.getFullPathName() << (floatOut ? " (32-bit float, for --compare)" : " (16-bit PCM)") << "\n";
    out << "settings: noise reduction " << (settings.nrEnabled ? "ON" : "OFF") << ", background " << juce::String (settings.nrBackground, 2) << ", impact "
        << juce::String (settings.nrImpact, 2) << ", EQ " << (settings.eqEnabled ? "ON" : "OFF");

    if (settings.eqEnabled)
        out << " output gain " << juce::String (settings.eqOutputGainDb, 1) << "dB";

    if (settings.eqEnabled)
        for (const auto& b : settings.eqBands)
            out << " [" << (b.on ? "" : "off ") << eqTypeId (b.type) << " " << juce::String (b.hz, 0) << "Hz " << juce::String (b.gainDb, 1) << "dB Q"
                << juce::String (b.q, 2) << "]";

    out << "\nengine: preset normal, gain 0 dB, block " << juce::String (result.blocks.empty() ? 0 : result.blocks.front().blockSamples) << " samples\n";
    out << "noise reduction latency: " << result.latencySamples << " samples (" << juce::String (1000.0 * result.latencySamples / in.sampleRate, 2)
        << " ms); removed from the output (the output is aligned with the input)\n";
    out << "error flags: " << (int) result.errorFlags << ", output FIFO underflows: " << result.underflowCount << "\n";

    // 区間ごとのゲート・インパクト抑制の統計（入力の区間。区間ファイルがなければ入力を自動判定）。
    std::vector<Segment> segs;

    if (parsed.options.containsKey ("--segments"))
    {
        if (! loadSegmentsFile (parsed.options["--segments"], segs, error))
        {
            std::fprintf (stderr, "%s\n", error.toRawUTF8());
            return 2;
        }

        warnSegmentsBeyond (segs, (double) in.samples.size() / in.sampleRate, "--segments");
    }
    else
        segs = detectSegments (in.samples, in.sampleRate);

    if (settings.nrEnabled && ! segs.empty())
    {
        out << "\ngate / impact statistics per segment (output-time samples, aligned to the input timeline)\n";
        out << " #  time(s)       kind             gate open [%]  impact attenuation active [%]\n";

        for (size_t i = 0; i < segs.size(); ++i)
        {
            long total = 0, closed = 0, impact = 0;

            for (const auto& b : result.blocks)
            {
                const double mid = b.startSec + 0.5 * (double) b.blockSamples / in.sampleRate;

                if (mid >= segs[i].startSec && mid < segs[i].endSec)
                {
                    total += b.blockSamples;
                    closed += b.gateClosedSamples;
                    impact += b.impactSamples;
                }
            }

            out << juce::String ((int) i + 1).paddedLeft (' ', 2) << "  " << timeRange (segs[i]) << " " << juce::String (segKindId (segs[i].kind)).paddedRight (' ', 15)
                << (total > 0 ? juce::String (100.0 * (double) (total - closed) / (double) total, 1).paddedLeft (' ', 10) : juce::String ("n/a").paddedLeft (' ', 10))
                << (total > 0 ? juce::String (100.0 * (double) impact / (double) total, 1).paddedLeft (' ', 20) : juce::String ("n/a").paddedLeft (' ', 20)) << "\n";
        }
    }

    if (parsed.options.containsKey ("--trace"))
    {
        juce::String csv ("time_s,input_rms_dbfs,output_rms_dbfs,gate_open_fraction,impact_attenuated_fraction\n");

        for (const auto& b : result.blocks)
        {
            const long a = (long) std::lround (b.startSec * in.sampleRate);

            if (a + b.blockSamples <= 0 || a >= (long) in.samples.size())
                continue;

            csv << juce::String (b.startSec, 3) << "," << juce::String (rmsOf (in.samples, a, a + b.blockSamples), 1) << ","
                << juce::String (rmsOf (result.output, a, a + b.blockSamples), 1) << "," << juce::String (1.0 - (double) b.gateClosedSamples / b.blockSamples, 3) << ","
                << juce::String ((double) b.impactSamples / b.blockSamples, 3) << "\n";
        }

        if (! cwd.getChildFile (parsed.options["--trace"]).replaceWithText (csv))
        {
            std::fprintf (stderr, "cannot write trace: %s\n", parsed.options["--trace"].toRawUTF8());
            return 2;
        }
    }

    print (out);
    return 0;
}

int runCompare (const juce::StringArray& args)
{
    const auto parsed = parseArgs (args, { "--segments", "--sonar-segments" });
    const char* usage = "usage: VoiceChangeTests --compare <raw.wav> <sonar.wav> <ours.wav> [--segments <file>] [--sonar-segments <file>]\n";

    if (! parsed.error.isEmpty() || parsed.positional.size() != 3)
    {
        printError (parsed.error, usage);
        return 2;
    }

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    std::array<Audio, 3> audio;
    juce::String error;

    for (int i = 0; i < 3; ++i)
        if (! readWav (cwd.getChildFile (parsed.positional[i]), audio[(size_t) i], error))
        {
            std::fprintf (stderr, "%s\n", error.toRawUTF8());
            return 2;
        }

    if (! juce::approximatelyEqual (audio[1].sampleRate, audio[0].sampleRate) || ! juce::approximatelyEqual (audio[2].sampleRate, audio[0].sampleRate))
    {
        std::fprintf (stderr, "sample rates differ (%.0f / %.0f / %.0f Hz); convert them to the same rate first\n", audio[0].sampleRate, audio[1].sampleRate,
                      audio[2].sampleRate);
        return 2;
    }

    std::vector<Segment> segs;

    if (parsed.options.containsKey ("--segments") && ! loadSegmentsFile (parsed.options["--segments"], segs, error))
    {
        std::fprintf (stderr, "%s\n", error.toRawUTF8());
        return 2;
    }

    warnSegmentsBeyond (segs, (double) audio[0].samples.size() / audio[0].sampleRate, "--segments");

    std::vector<Segment> sonarSegs;

    if (parsed.options.containsKey ("--sonar-segments") && ! loadSegmentsFile (parsed.options["--sonar-segments"], sonarSegs, error))
    {
        std::fprintf (stderr, "%s\n", error.toRawUTF8());
        return 2;
    }

    warnSegmentsBeyond (sonarSegs, (double) audio[1].samples.size() / audio[1].sampleRate, "--sonar-segments");

    const auto result = compareRecordings (audio[0].samples, audio[1].samples, audio[2].samples, audio[0].sampleRate,
                                           parsed.options.containsKey ("--segments") ? &segs : nullptr,
                                           parsed.options.containsKey ("--sonar-segments") ? &sonarSegs : nullptr);
    print (formatReport (result));
    return 0;
}

} // namespace vc::rectool
