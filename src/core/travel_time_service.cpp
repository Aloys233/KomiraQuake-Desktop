#include "core/travel_time_service.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/quake_calculator.h"

namespace komira {

namespace {
std::vector<double> toDoubleVector(const QJsonArray& arr) {
    std::vector<double> out;
    out.reserve(static_cast<size_t>(arr.size()));
    for (const auto& v : arr) out.push_back(v.toDouble());
    return out;
}

std::vector<std::vector<double>> toMatrix(const QJsonArray& arr) {
    std::vector<std::vector<double>> out;
    out.reserve(static_cast<size_t>(arr.size()));
    for (const auto& row : arr) out.push_back(toDoubleVector(row.toArray()));
    return out;
}
} // namespace

TravelTimeService& TravelTimeService::instance() {
    static TravelTimeService svc;
    return svc;
}

void TravelTimeService::loadFromJson(const QByteArray& raw) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    const QJsonObject root = doc.object();
    QHash<QString, TravelTable> parsed;
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject table = it.value().toObject();
        parsed.insert(it.key(),
                      TravelTable(toDoubleVector(table.value("depths").toArray()),
                                  toDoubleVector(table.value("distances").toArray()),
                                  toMatrix(table.value("p_times").toArray()),
                                  toMatrix(table.value("s_times").toArray())));
    }
    tables_ = parsed;
}

bool TravelTimeService::loadFromFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    loadFromJson(file.readAll());
    return isLoaded();
}

bool TravelTimeService::isLoaded() const { return !tables_.isEmpty(); }

std::pair<double, double> TravelTimeService::estimate(double depthKm,
                                                     double distanceKm,
                                                     const QString& table) const {
    const auto it = tables_.constFind(table);
    if (it == tables_.constEnd() || !it->valid()) {
        return QuakeCalculator::estimateTravelTimes(distanceKm, depthKm);
    }
    return {it->estimateP(depthKm, distanceKm), it->estimateS(depthKm, distanceKm)};
}

double TravelTimeService::distanceForTime(double depthKm,
                                         double seconds,
                                         bool isPWave,
                                         const QString& table) const {
    const auto it = tables_.constFind(table);
    if (it == tables_.constEnd() || !it->valid()) {
        return seconds * (isPWave ? QuakeCalculator::kPWaveSpeed : QuakeCalculator::kSWaveSpeed);
    }
    return it->distanceForTime(depthKm, seconds, isPWave);
}

} // namespace komira
