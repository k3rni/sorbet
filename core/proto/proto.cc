#include "core/proto/proto.h"
#include "common/Random.h"
#include "core/Counters_impl.h"

#include "absl/strings/str_cat.h"

#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/util/type_resolver_util.h>

using namespace std;

namespace sorbet {
namespace core {

com::stripe::rubytyper::Name Proto::toProto(const GlobalState &gs, NameRef name) {
    com::stripe::rubytyper::Name protoName;
    protoName.set_name(name.show(gs));
    switch (name.data(gs).kind) {
        case UTF8:
            protoName.set_kind(com::stripe::rubytyper::Name::UTF8);
        case UNIQUE:
            protoName.set_kind(com::stripe::rubytyper::Name::UNIQUE);
        case CONSTANT:
            protoName.set_kind(com::stripe::rubytyper::Name::CONSTANT);
    }
    return protoName;
}

com::stripe::rubytyper::Symbol Proto::toProto(const GlobalState &gs, SymbolRef sym) {
    com::stripe::rubytyper::Symbol symbolProto;
    const auto &data = sym.data(gs);

    symbolProto.set_id(sym._id);
    *symbolProto.mutable_name() = toProto(gs, data.name);

    if (data.isClass()) {
        symbolProto.set_kind(com::stripe::rubytyper::Symbol::CLASS);
    } else if (data.isStaticField()) {
        symbolProto.set_kind(com::stripe::rubytyper::Symbol::STATIC_FIELD);
    } else if (data.isField()) {
        symbolProto.set_kind(com::stripe::rubytyper::Symbol::FIELD);
    } else if (data.isMethod()) {
        symbolProto.set_kind(com::stripe::rubytyper::Symbol::METHOD);
    } else if (data.isMethodArgument()) {
        symbolProto.set_kind(com::stripe::rubytyper::Symbol::ARGUMENT);
    } else if (data.isTypeMember()) {
        symbolProto.set_kind(com::stripe::rubytyper::Symbol::TYPE_MEMBER);
    } else if (data.isTypeArgument()) {
        symbolProto.set_kind(com::stripe::rubytyper::Symbol::TYPE_ARGUMENT);
    }

    if (data.isClass() || data.isMethod()) {
        if (data.isClass()) {
            for (auto thing : data.mixins()) {
                symbolProto.add_mixins(thing._id);
            }
        } else {
            for (auto thing : data.arguments()) {
                symbolProto.add_arguments(thing._id);
            }
        }

        if (data.superClass.exists()) {
            symbolProto.set_superclass(data.superClass._id);
        }
    }

    vector<pair<absl::string_view, com::stripe::rubytyper::Symbol>> children;
    for (auto pair : data.members) {
        if (pair.first == Names::singleton() || pair.first == Names::attached() ||
            pair.first == Names::classMethods()) {
            continue;
        }

        if (!pair.second.exists()) {
            continue;
        }

        children.emplace_back(pair.first.data(gs).shortName(gs), toProto(gs, pair.second));
    }
    auto by_name = [](pair<absl::string_view, com::stripe::rubytyper::Symbol> const &a,
                      pair<absl::string_view, com::stripe::rubytyper::Symbol> const &b) { return a.first < b.first; };
    sort(children.begin(), children.end(), by_name);
    for (auto pair : children) {
        *symbolProto.add_children() = move(pair.second);
    }
    return symbolProto;
}

com::stripe::rubytyper::Loc Proto::toProto(const GlobalState &gs, Loc loc) {
    com::stripe::rubytyper::Loc protoLoc;
    auto position = protoLoc.mutable_position();
    auto start = position->mutable_start();
    auto end = position->mutable_end();

    if (!loc.exists()) {
        protoLoc.set_path("???");
    } else {
        auto path = loc.file.data(gs).path();
        protoLoc.set_path(string(path));

        auto pos = loc.position(gs);
        start->set_line(pos.first.line);
        start->set_column(pos.first.column);
        end->set_line(pos.second.line);
        end->set_column(pos.second.column);
    }

    return protoLoc;
}

com::stripe::payserver::events::cibot::SourceMetrics Proto::toProto(const CounterState &counters,
                                                                    absl::string_view prefix) {
    com::stripe::payserver::events::cibot::SourceMetrics metrics;
    auto unix_timestamp = chrono::seconds(time(nullptr));
    metrics.set_timestamp(unix_timestamp.count());

    // UUID version 1 as specified in RFC 4122
    string uuid =
        strprintf("%llx-%x-%x-%x-%llx%x",
                  (unsigned long long)Random::uniformU8(), // Generates a 64-bit Hex number
                  Random::uniformU4(),                     // Generates a 32-bit Hex number
                  Random::uniformU4(0, 0x0fff) |
                      0x4000, // Generates a 32-bit Hex number of the form 4xxx (4 indicates the UUID version)
                  Random::uniformU4(0, 0x3fff) | 0x8000, // Generates a 32-bit Hex number in the range [0x8000, 0xbfff]
                  (unsigned long long)Random::uniformU8(), rand()); // Generates a 96-bit Hex number

    metrics.set_uuid(uuid);

    auto canon = counters.counters->canonicalize();
    for (auto &cat : canon.counters_by_category) {
        CounterImpl::CounterType sum = 0;
        for (auto &e : cat.second) {
            sum += e.second;
            com::stripe::payserver::events::cibot::SourceMetrics_SourceMetricEntry *metric = metrics.add_metrics();
            metric->set_name(absl::StrCat(prefix, ".", cat.first, ".", e.first));
            metric->set_value(e.second);
        }

        com::stripe::payserver::events::cibot::SourceMetrics_SourceMetricEntry *metric = metrics.add_metrics();
        metric->set_name(absl::StrCat(prefix, ".", cat.first, ".total"));
        metric->set_value(sum);
    }

    for (auto &hist : canon.histograms) {
        CounterImpl::CounterType sum = 0;
        for (auto &e : hist.second) {
            sum += e.second;
        }
        CounterImpl::CounterType running = 0;
        vector<pair<int, bool>> percentiles = {{25, false}, {50, false}, {75, false}, {90, false}};
        for (auto &e : hist.second) {
            running += e.second;
            for (auto &pct : percentiles) {
                if (pct.second) {
                    continue;
                }
                if (running >= sum * pct.first / 100) {
                    pct.second = true;
                    com::stripe::payserver::events::cibot::SourceMetrics_SourceMetricEntry *metric =
                        metrics.add_metrics();
                    metric->set_name(absl::StrCat(prefix, ".", hist.first, ".p", pct.first));
                    metric->set_value(e.first);
                }
            }
        }
        com::stripe::payserver::events::cibot::SourceMetrics_SourceMetricEntry *metric = metrics.add_metrics();
        metric->set_name(absl::StrCat(prefix, ".", hist.first, ".min"));
        metric->set_value(hist.second.begin()->first);

        metric = metrics.add_metrics();
        metric->set_name(absl::StrCat(prefix, ".", hist.first, ".max"));
        metric->set_value((--hist.second.end())->first);

        metric = metrics.add_metrics();
        metric->set_name(absl::StrCat(prefix, ".", hist.first, ".total"));
        metric->set_value(sum);
    }

    for (auto &e : canon.counters) {
        com::stripe::payserver::events::cibot::SourceMetrics_SourceMetricEntry *metric = metrics.add_metrics();
        metric->set_name(absl::StrCat(prefix, ".", e.first));
        metric->set_value(e.second);
    }
    return metrics;
}

com::stripe::rubytyper::File::StrictLevel strictToProto(core::StrictLevel strict) {
    switch (strict) {
        case core::StrictLevel::Stripe:
            return com::stripe::rubytyper::File::StrictLevel::File_StrictLevel_Stripe;
        case core::StrictLevel::Typed:
            return com::stripe::rubytyper::File::StrictLevel::File_StrictLevel_Typed;
        case core::StrictLevel::Strict:
            return com::stripe::rubytyper::File::StrictLevel::File_StrictLevel_Strict;
        case core::StrictLevel::Strong:
            return com::stripe::rubytyper::File::StrictLevel::File_StrictLevel_Strong;
        case core::StrictLevel::Autogenerated:
            return com::stripe::rubytyper::File::StrictLevel::File_StrictLevel_Autogenerated;
        default:
            ENFORCE(false, "bad strict level: ", (int)strict);
    }
}

com::stripe::rubytyper::FileTable Proto::filesToProto(const GlobalState &gs) {
    com::stripe::rubytyper::FileTable files;
    for (int i = 1; i < gs.filesUsed(); ++i) {
        core::FileRef file(i);
        auto *entry = files.add_files();
        auto path_view = file.data(gs).path();
        string path(path_view.begin(), path_view.end());
        entry->set_path(path);
        entry->set_sigil(strictToProto(file.data(gs).sigil));
        entry->set_strict(strictToProto(file.data(gs).strict));
        entry->set_had_errors(file.data(gs).hadErrors());
    }
    return files;
}

string Proto::toJSON(const google::protobuf::Message &message) {
    string json_string;
    google::protobuf::util::JsonPrintOptions options;
    options.add_whitespace = true;
    options.always_print_primitive_fields = false;
    options.preserve_proto_field_names = true;
    google::protobuf::util::MessageToJsonString(message, &json_string, options);
    return json_string;
}

const char *kTypeUrlPrefix = "type.googleapis.com";

void Proto::toJSON(const google::protobuf::Message &message, std::ostream &out) {
    string binaryProto;
    message.SerializeToString(&binaryProto);

    google::protobuf::io::ArrayInputStream istream(binaryProto.data(), binaryProto.size());
    google::protobuf::io::OstreamOutputStream ostream(&out);

    google::protobuf::util::JsonPrintOptions options;
    options.add_whitespace = true;
    options.always_print_primitive_fields = false;
    options.preserve_proto_field_names = true;

    const google::protobuf::DescriptorPool *pool = message.GetDescriptor()->file()->pool();
    unique_ptr<google::protobuf::util::TypeResolver> resolver(
        google::protobuf::util::NewTypeResolverForDescriptorPool(kTypeUrlPrefix, pool));

    string url = absl::StrCat(kTypeUrlPrefix, "/", message.GetDescriptor()->full_name());
    auto status = google::protobuf::util::BinaryToJsonStream(resolver.get(), url, &istream, &ostream, options);
    if (!status.ok()) {
        cerr << "error converting to proto json: " << status.error_message() << "\n";
        abort();
    }
}

} // namespace core
} // namespace sorbet
