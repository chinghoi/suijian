#include "ColorPipeline.h"
#include <QJsonArray>
#include <cmath>

namespace {
double luminance(const QJsonValue &value) {
    if (value.isDouble()) return value.toDouble();
    const auto parts = value.toString().split('/');
    bool ok = false;
    const double numerator = parts.value(0).toDouble(&ok);
    if (!ok) return 0;
    if (parts.size() == 1) return numerator;
    const double denominator = parts.value(1).toDouble(&ok);
    return ok && denominator > 0 ? numerator / denominator : 0;
}
}

color::SdrConversion color::toSdr(const QJsonObject &stream, bool hasToneMapping) {
    const QString transfer = stream.value("color_transfer").toString();
    if (transfer != "arib-std-b67" && transfer != "smpte2084") return {};
    if (!hasToneMapping) return {{}, "当前 FFmpeg 缺少 zscale 或 tonemap，无法转换 HDR；请使用包含这两个滤镜的 FFmpeg", false};
    if (stream.value("color_primaries").toString() != "bt2020"
        || stream.value("color_space").toString() != "bt2020nc") {
        return {{}, "HDR 素材缺少受支持的 BT.2020 色域或矩阵标记，无法安全转换为 SDR", false};
    }
    const auto range = stream.value("color_range").toString();
    if (range != "tv" && range != "pc") {
        return {{}, "HDR 素材未标明完整/有限色阶范围，无法安全转换为 SDR", false};
    }

    // Reference display peak for HLG, or fallback for PQ with no mastering data.
    double peakNits = 1000;
    if (transfer == "smpte2084") {
        double maxContent = 0, masteringPeak = 0;
        for (const auto &side : stream.value("side_data_list").toArray()) {
            const auto object = side.toObject();
            if (object.value("side_data_type").toString() == "Content light level metadata")
                maxContent = luminance(object.value("max_content"));
            if (object.value("side_data_type").toString() == "Mastering display metadata")
                masteringPeak = luminance(object.value("max_luminance"));
        }
        const auto validPeak = [](double value) { return std::isfinite(value) && value >= 100 && value <= 10000; };
        if (validPeak(maxContent)) peakNits = maxContent;
        else if (validPeak(masteringPeak)) peakNits = masteringPeak;
    }
    // Newer zimg rejects linear YUV and an unspecified chroma location.
    // Tag the frame first, then convert to linear RGB. Keep a stated chroma
    // location; do not invent one.
    QString params = QString("setparams=color_primaries=bt2020:color_trc=%1:colorspace=bt2020nc:range=%2")
        .arg(transfer, range);
    const QString chroma = stream.value("chroma_location").toString();
    if (chroma == "left" || chroma == "center" || chroma == "topleft"
        || chroma == "top" || chroma == "bottomleft" || chroma == "bottom") {
        params += ":chroma_location=" + chroma;
    }
    const QString filter = QString(
        "%1,zscale=rin=%2:t=linear:npl=100:m=gbr,"
        "format=gbrpf32le,zscale=p=bt709,"
        "tonemap=tonemap=mobius:param=0.3:desat=2:peak=%3,"
        "zscale=t=bt709:m=bt709:r=limited:dither=error_diffusion,format=yuv420p,"
        "sidedata=mode=delete:type=MASTERING_DISPLAY_METADATA,"
        "sidedata=mode=delete:type=CONTENT_LIGHT_LEVEL")
        .arg(params, range == "pc" ? "full" : "limited")
        .arg(peakNits / 100.0, 0, 'g', 8);
    return {filter, {}, true};
}
