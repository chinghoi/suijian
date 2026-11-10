#pragma once
#include <QJsonObject>
#include <QString>

namespace color {
struct SdrConversion {
    QString filter;
    QString error;
    bool toneMapped = false;
};
// FFmpeg must provide zscale and tonemap for tagged HLG/PQ inputs.
// SDR/unknown input transfer is unchanged; D-Log is not inferred from bit depth.
SdrConversion toSdr(const QJsonObject &stream, bool hasToneMapping);
}
