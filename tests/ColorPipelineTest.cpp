#include "ColorPipeline.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QDebug>
#include <stdexcept>

static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static QByteArray run(const QString &program, const QStringList &args) {
    QProcess process;
    process.start(program, args);
    require(process.waitForFinished(30000), "media command timed out or failed to start");
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning().noquote() << process.readAllStandardError();
        throw std::runtime_error("media command failed");
    }
    return process.readAllStandardOutput();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory unavailable");
    QJsonObject hdr{{"color_transfer", "arib-std-b67"}, {"color_space", "bt2020nc"},
                    {"color_primaries", "bt2020"}, {"color_range", "tv"}};
    require(!color::toSdr(hdr, false).error.isEmpty(), "missing filters must be reported");
    auto unknown = hdr; unknown.remove("color_range");
    require(!color::toSdr(unknown, true).error.isEmpty(), "unknown HDR range must not be guessed");
    unknown = hdr; unknown["color_primaries"] = "bt709";
    require(!color::toSdr(unknown, true).error.isEmpty(), "unsupported HDR gamut must be reported");
    require(color::toSdr(QJsonObject{{"pix_fmt", "yuv420p10le"}}, false).filter.isEmpty(),
            "10-bit alone must not trigger HDR conversion");
    auto pq = hdr; pq["color_transfer"] = "smpte2084";
    pq["side_data_list"] = QJsonArray{QJsonObject{{"side_data_type", "Mastering display metadata"},
                                               {"max_luminance", "40000000/10000"}}};
    require(color::toSdr(pq, true).filter.contains("peak=40,"), "mastering luminance was not used");
    hdr["color_range"] = "pc";
    require(color::toSdr(hdr, true).filter.contains("rin=full:"), "full range was not respected");
    hdr["color_range"] = "tv";
    hdr["chroma_location"] = "left";

    // Real FFmpeg regression on neutral HDR ramps: tags, depth, monotonicity
    // and useful output range. This checks transforms rather than just strings.
    for (const auto &transfer : {QString("arib-std-b67"), QString("smpte2084")}) {
        const auto source = directory.path()+"/"+transfer+".mkv";
        const auto output = directory.path()+"/"+transfer+"-sdr.mkv";
        run("ffmpeg", {"-v","error","-y","-f","lavfi","-i",
            "nullsrc=s=128x72:r=1,format=yuv420p10le,geq=lum='64+876*X/(W-1)':cb=512:cr=512",
            "-frames:v","1","-c:v","ffv1","-color_primaries","bt2020","-color_trc",transfer,
            "-colorspace","bt2020nc","-color_range","tv",source});
        hdr["color_transfer"] = transfer;
        const auto conversion = color::toSdr(hdr, true);
        require(conversion.error.isEmpty() && conversion.toneMapped, "tagged HDR was not converted");
        require(conversion.filter.contains("chromalin=left")
                && !conversion.filter.contains("range=tv:chroma_location"),
                "chroma location must use the zscale option supported by FFmpeg 6");
        run("ffmpeg", {"-v","error","-y","-i",source,"-vf",conversion.filter,
            "-c:v","ffv1","-color_primaries","bt709","-color_trc","bt709",
            "-colorspace","bt709","-color_range","tv",output});
        auto info = QJsonDocument::fromJson(run("ffprobe", {"-v","error","-show_streams","-of","json",output}));
        const auto stream = info.object()["streams"].toArray().first().toObject();
        require(stream["pix_fmt"] == "yuv420p" && stream["color_transfer"] == "bt709"
                && stream["color_primaries"] == "bt709" && stream["color_space"] == "bt709"
                && stream["color_range"] == "tv", "SDR output metadata/depth incorrect");
        const auto frame = run("ffmpeg", {"-v","error","-i",output,"-frames:v","1","-pix_fmt","gray","-f","rawvideo","pipe:1"});
        require(frame.size() == 128*72, "unexpected output frame dimensions");
        const auto *values = reinterpret_cast<const unsigned char *>(frame.constData());
        require(values[0] < 5 && values[127] > 180, "tone-mapped ramp has no useful dynamic range");
        for (int x=1; x<128; ++x) require(values[x]+2 >= values[x-1], "neutral ramp is not monotonic");

        const auto composed = directory.path()+"/"+transfer+"-composed.mkv";
        const QString graph = QString("[0:v]%1,split=2[fgin][bgin];"
            "[bgin]scale=160:90:force_original_aspect_ratio=increase,crop=160:90,"
            "boxblur=8:2,scale=320:180:flags=fast_bilinear,"
            "eq=brightness=-0.08:saturation=1.15[bg];"
            "[fgin]scale=320:180:force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1[fg];"
            "[bg][fg]overlay=(W-w)/2:(H-h)/2:format=yuv420,setsar=1[outv]").arg(conversion.filter);
        run("ffmpeg", {"-v","error","-y","-i",source,"-filter_complex",graph,
            "-map","[outv]","-c:v","ffv1",composed});
    }
    qInfo() << "HDR to SDR conversion checks passed";
}
