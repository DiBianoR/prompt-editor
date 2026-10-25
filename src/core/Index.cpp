#include "Index.h"

#include <stdexcept>

#include <nlohmann/json.hpp>

namespace pe {

using json = nlohmann::ordered_json;

namespace {

const char* const kStatusNames[kAnchorStatusCount] = {
    "exact", "moved", "normalized", "fuzzy", "ambiguous", "orphaned"};

std::string optString(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

Range parseRange(const json& j) {
    Range r;
    r.start = j.at("start").get<size_t>();
    r.end = j.at("end").get<size_t>();
    r.exact = j.at("exact").get<std::string>();
    r.prefix = optString(j, "prefix");
    r.suffix = optString(j, "suffix");
    r.touched = j.value("touched", false);
    r.status = statusFromString(optString(j, "status"));
    if (r.end < r.start) throw std::runtime_error("range end before start");
    return r;
}

Topic parseTopic(const json& j, const std::string& inheritedColor) {
    Topic t;
    t.id = j.at("id").get<std::string>();
    t.name = j.at("name").get<std::string>();
    t.color = optString(j, "color");
    if (t.color.empty()) t.color = inheritedColor;
    t.note = optString(j, "note");
    t.analysis = optString(j, "analysis");
    if (auto it = j.find("ranges"); it != j.end())
        for (const json& r : *it) t.ranges.push_back(parseRange(r));
    return t;
}

json rangeJson(const Range& r) {
    json j;
    j["start"] = r.start;
    j["end"] = r.end;
    j["exact"] = r.exact;
    j["prefix"] = r.prefix;
    j["suffix"] = r.suffix;
    j["touched"] = r.touched;
    // Only statuses that still mean something after a save are written.
    if (needsReview(r.status)) j["status"] = toString(r.status);
    return j;
}

json topicJson(const Topic& t, bool withSubtopics) {
    json j;
    j["id"] = t.id;
    j["name"] = t.name;
    j["color"] = t.color;
    if (!t.note.empty()) j["note"] = t.note;
    if (!t.analysis.empty()) j["analysis"] = t.analysis;
    j["ranges"] = json::array();
    for (const Range& r : t.ranges) j["ranges"].push_back(rangeJson(r));
    if (withSubtopics) {
        j["subtopics"] = json::array();
        for (const Topic& s : t.subtopics) j["subtopics"].push_back(topicJson(s, false));
    }
    return j;
}

}  // namespace

const char* toString(AnchorStatus status) {
    return kStatusNames[static_cast<int>(status)];
}

AnchorStatus statusFromString(const std::string& s) {
    for (int i = 0; i < kAnchorStatusCount; ++i)
        if (s == kStatusNames[i]) return static_cast<AnchorStatus>(i);
    return AnchorStatus::Exact;
}

bool needsReview(AnchorStatus status) {
    return status == AnchorStatus::Fuzzy || status == AnchorStatus::Ambiguous ||
           status == AnchorStatus::Orphaned;
}

Index parseIndex(const std::string& text) {
    try {
        json j = json::parse(text);
        Index index;
        index.version = j.value("version", 1);
        index.document = optString(j, "document");
        index.documentHash = optString(j, "documentHash");
        for (const json& tj : j.at("topics")) {
            Topic t = parseTopic(tj, "#90A4AE");
            if (auto it = tj.find("subtopics"); it != tj.end())
                for (const json& sj : *it) t.subtopics.push_back(parseTopic(sj, t.color));
            index.topics.push_back(std::move(t));
        }
        if (auto it = j.find("unindexed"); it != j.end())
            for (const json& g : *it)
                index.unindexed.push_back({g.at("start").get<size_t>(), g.at("end").get<size_t>(),
                                           optString(g, "exact")});
        return index;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("index: ") + e.what());
    }
}

std::string serializeIndex(const Index& index) {
    json j;
    j["version"] = index.version;
    j["document"] = index.document;
    j["documentHash"] = index.documentHash;
    j["offsetUnit"] = "utf8-byte";
    j["topics"] = json::array();
    for (const Topic& t : index.topics) j["topics"].push_back(topicJson(t, true));
    j["unindexed"] = json::array();
    for (const Gap& g : index.unindexed)
        j["unindexed"].push_back({{"start", g.start}, {"end", g.end}, {"exact", g.exact}});
    return j.dump(2) + "\n";
}

}  // namespace pe
