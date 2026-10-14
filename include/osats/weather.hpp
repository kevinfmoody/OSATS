#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>

namespace osats {

// Lightweight METAR provider. Starts with a built-in sample book and accepts
// runtime overrides — enough for clients/demos without an external dependency.
class WeatherService {
 public:
  WeatherService() {
    samples_ = {
        {"KSFO",
         "KSFO 141556Z 28012KT 10SM FEW018 SCT120 18/10 A3002 RMK AO2 "
         "OSATS DEMO"},
        {"KLAX",
         "KLAX 141553Z 25008KT 10SM SCT015 BKN250 22/14 A2996 RMK AO2 "
         "OSATS DEMO"},
        {"KJFK",
         "KJFK 141551Z 31014G22KT 10SM BKN040 16/05 A2990 RMK AO2 OSATS DEMO"},
        {"EGLL",
         "EGLL 141550Z 25010KT 9999 SCT025 BKN040 14/09 Q1015 OSATS DEMO"},
        {"LFPG",
         "LFPG 141530Z 22008KT CAVOK 17/08 Q1018 NOSIG OSATS DEMO"},
    };
  }

  std::string metarFor(std::string_view icao) const {
    std::string key(icao);
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto it = samples_.find(key);
    if (it != samples_.end()) return it->second;
    return key + " 000000Z 00000KT 9999 NSC 15/10 Q1013 OSATS GENERATED";
  }

  std::string atisFor(std::string_view icao) const {
    std::string key(icao);
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return key + " ATIS INFO A. " + metarFor(key) +
           ". ADVISE ON INITIAL CONTACT YOU HAVE INFORMATION A.";
  }

 private:
  std::unordered_map<std::string, std::string> samples_;
};

}  // namespace osats
