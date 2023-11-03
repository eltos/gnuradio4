#ifndef GNURADIO_TAGMONITORS_HPP
#define GNURADIO_TAGMONITORS_HPP

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/reflection.hpp>
#include <gnuradio-4.0/Tag.hpp>

namespace gr::testing {

enum class ProcessFunction {
    USE_PROCESS_BULK     = 0, ///
    USE_PROCESS_ONE      = 1, ///
    USE_PROCESS_ONE_SIMD = 2  ///
};

void
print_tag(const Tag &tag, std::string_view prefix = {}) {
    if (tag.map.empty()) {
        fmt::print("{} @index= {}: map: {{ <empty map> }}\n", prefix, tag.index);
        return;
    }
    fmt::print("{} @index= {}: map: {{ {} }}\n", prefix, tag.index, fmt::join(tag.map, ", "));
}

template<typename MapType>
void
map_diff_report(const MapType &map1, const MapType &map2, const std::string &name1, const std::string &name2, const std::optional<std::string> &ignoreKey = std::nullopt) {
    for (const auto &[key, value] : map1) {
        if (ignoreKey && key == *ignoreKey) {
            continue; // skip this key
        }
        const auto it = map2.find(key);
        if (it == map2.end()) {
            fmt::print("    key '{}' is present in {} but not in {}\n", key, name1, name2);
        } else if (it->second != value) {
            fmt::print("    key '{}' has different values ('{}' vs '{}')\n", key, value, it->second);
        }
    }
}

template<typename IterType>
void
mismatch_report(const IterType &mismatchedTag1, const IterType &mismatchedTag2, const IterType &tags1_begin, const std::optional<std::string> &ignoreKey = std::nullopt) {
    const auto index = static_cast<size_t>(std::distance(tags1_begin, mismatchedTag1));
    fmt::print("mismatch at index {}", index);
    if (mismatchedTag1->index != mismatchedTag2->index) {
        fmt::print("  - different index: {} vs {}\n", mismatchedTag1->index, mismatchedTag2->index);
    }

    if (mismatchedTag1->map != mismatchedTag2->map) {
        fmt::print("  - different map content:\n");
        map_diff_report(mismatchedTag1->map, mismatchedTag2->map, "the first map", "the second", ignoreKey);
        map_diff_report(mismatchedTag2->map, mismatchedTag1->map, "the second map", "the first", ignoreKey);
    }
}

bool
equal_tag_lists(const std::vector<Tag> &tags1, const std::vector<Tag> &tags2, const std::optional<std::string> &ignoreKey = std::nullopt) {
    if (tags1.size() != tags2.size()) {
        fmt::println("vectors have different sizes ({} vs {})\n", tags1.size(), tags2.size());
        return false;
    }

    auto customComparator = [&ignoreKey](const Tag &tag1, const Tag &tag2) {
        if (ignoreKey) {
            // make a copy of the maps to compare without the ignored key
            auto map1 = tag1.map;
            auto map2 = tag2.map;
            map1.erase(*ignoreKey);
            map2.erase(*ignoreKey);
            return map1 == map2;
        }
        return tag1 == tag2; // Use Tag's equality operator
    };

    auto [mismatchedTag1, mismatchedTag2] = std::mismatch(tags1.begin(), tags1.end(), tags2.begin(), customComparator);
    if (mismatchedTag1 != tags1.end()) {
        mismatch_report(mismatchedTag1, mismatchedTag2, tags1.begin(), ignoreKey);
        return false;
    }
    return true;
}

template<typename T, ProcessFunction UseProcessVariant = ProcessFunction::USE_PROCESS_BULK>
struct TagSource : public Block<TagSource<T, UseProcessVariant>> {
    PortOut<T>       out;
    std::vector<Tag> tags{};
    std::size_t      next_tag{ 0 };
    std::uint64_t    n_samples_max{ 1024 };
    std::uint64_t    n_samples_produced{ 0 };
    float            sample_rate     = 1000.0f;
    std::string      signal_name     = "unknown signal";
    bool             verbose_console = false;
    bool             mark_tag        = true; // true: mark tagged samples with '1' or '0' otherwise. false: [0, 1, 2, ..., ]

    void
    start() {
        n_samples_produced = 0U;
    }

    T
    processOne(std::size_t offset) noexcept
        requires(UseProcessVariant == ProcessFunction::USE_PROCESS_ONE)
    {
        const bool generatedTag = generateTag("processOne(...)", offset);
        n_samples_produced++;
        if (n_samples_produced >= n_samples_max) {
            fmt::println("terminate n_samples_produced {}", n_samples_produced);
            this->requestStop();
        }
        return mark_tag ? (generatedTag ? static_cast<T>(1) : static_cast<T>(0)) : static_cast<T>(n_samples_produced);
    }

    work::Status
    processBulk(PublishableSpan auto &output) noexcept
        requires(UseProcessVariant == ProcessFunction::USE_PROCESS_BULK)
    {
        const bool             generatedTag = generateTag("processBulk(...)");
        Tag::signed_index_type nextTagIn    = next_tag < tags.size() ? std::max(1L, tags[next_tag].index - static_cast<Tag::signed_index_type>(n_samples_produced))
                                                                     : static_cast<Tag::signed_index_type>(n_samples_max - n_samples_produced);
        const std::size_t nSamples = n_samples_produced < n_samples_max ? std::min(static_cast<std::size_t>(std::max(1L, nextTagIn)), output.size()) : 0UZ; // '0UZ' -> DONE, produced enough samples

        if (mark_tag) {
            output[0] = generatedTag ? static_cast<T>(1) : static_cast<T>(0);
        } else {
            for (std::size_t i = 0; i < nSamples; ++i) {
                output[i] = static_cast<T>(n_samples_produced + i);
            }
        }

        n_samples_produced += static_cast<std::uint64_t>(nSamples);
        output.publish(nSamples);
        return n_samples_produced < n_samples_max ? work::Status::OK : work::Status::DONE;
    }

private:
    bool
    generateTag(std::string_view processFunctionName, std::size_t offset = 0) {
        if (next_tag < tags.size() && tags[next_tag].index <= static_cast<Tag::signed_index_type>(n_samples_produced)) {
            if (verbose_console) {
                print_tag(tags[next_tag], fmt::format("{}::{}\t publish tag at  {:6}", this->name.value, processFunctionName, n_samples_produced));
            }
            out.publishTag(tags[next_tag].map, offset); // indices > 0 write tags in the future ... handle with care
            this->_output_tags_changed = true;
            next_tag++;
            return true;
        }
        return false;
    }
};

template<typename T, ProcessFunction UseProcessVariant>
struct TagMonitor : public Block<TagMonitor<T, UseProcessVariant>> {
    using ClockSourceType = std::chrono::system_clock;
    PortIn<T>                                in;
    PortOut<T>                               out;
    std::vector<T>                           samples{};
    std::vector<Tag>                         tags{};
    std::uint64_t                            n_samples_expected{ 0 };
    std::uint64_t                            n_samples_produced{ 0 };
    float                                    sample_rate = 1000.0f;
    std::string                              signal_name;
    bool                                     log_tags         = true;
    bool                                     log_samples      = true;
    bool                                     verbose_console  = false;
    std::chrono::time_point<ClockSourceType> _timeFirstSample = ClockSourceType::now();
    std::chrono::time_point<ClockSourceType> _timeLastSample  = ClockSourceType::now();

    void
    start() {
        if (verbose_console) {
            fmt::println("started TagMonitor {} aka. '{}'", this->unique_name, this->name);
        }
        _timeFirstSample = ClockSourceType::now();
        samples.clear();
        if (log_samples) {
            samples.reserve(std::max(0UZ, static_cast<std::size_t>(n_samples_expected)));
        }
        tags.clear();
    }

    constexpr T
    processOne(const T &input) noexcept
        requires(UseProcessVariant == ProcessFunction::USE_PROCESS_ONE)
    {
        if (this->input_tags_present()) {
            const Tag &tag = this->mergedInputTag();
            if (verbose_console) {
                print_tag(tag, fmt::format("{}::processOne(...)\t received tag at {:6}", this->name, n_samples_produced));
            }
            tags.emplace_back(n_samples_produced, tag.map);
        }
        if (log_samples) {
            samples.emplace_back(input);
        }
        n_samples_produced++;
        return input;
    }

    template<gr::meta::t_or_simd<T> V>
    [[nodiscard]] constexpr V
    processOne(const V &input) noexcept // to note: the SIMD-version does not support adding tags mid-way since this is chunked at V::size()
        requires(UseProcessVariant == ProcessFunction::USE_PROCESS_ONE_SIMD)
    {
        if (this->input_tags_present()) {
            const Tag &tag = this->mergedInputTag();
            if (verbose_console) {
                print_tag(tag, fmt::format("{}::processOne(...)\t received tag at {:6}", this->name, n_samples_produced));
            }
            tags.emplace_back(n_samples_produced, tag.map);
        }
        if (log_samples) {
            if constexpr (gr::meta::any_simd<V>) {
                alignas(stdx::memory_alignment_v<stdx::native_simd<T>>) std::array<T, V::size()> mem = {};
                input.copy_to(&mem[0], stdx::vector_aligned);
                samples.insert(samples.end(), mem.begin(), mem.end());
            } else {
                samples.emplace_back(input);
            }
        }
        if constexpr (gr::meta::any_simd<V>) {
            n_samples_produced += static_cast<std::uint64_t>(V::size());
        } else {
            n_samples_produced++;
        }
        return input;
    }

    constexpr work::Status
    processBulk(std::span<const T> input, std::span<T> output) noexcept
        requires(UseProcessVariant == ProcessFunction::USE_PROCESS_BULK)
    {
        if (log_tags && this->input_tags_present()) {
            const Tag &tag = this->mergedInputTag();
            if (verbose_console) {
                print_tag(tag, fmt::format("{}::processBulk(...{}, ...{})\t received tag at {:6}", this->name, input.size(), output.size(), n_samples_produced));
            }
            tags.emplace_back(n_samples_produced, tag.map);
        }

        if (log_samples) {
            samples.insert(samples.cend(), input.begin(), input.end());
        }

        n_samples_produced += static_cast<std::uint64_t>(input.size());
        std::memcpy(output.data(), input.data(), input.size() * sizeof(T));

        return work::Status::OK;
    }
};

template<typename T, ProcessFunction UseProcessVariant>
struct TagSink : public Block<TagSink<T, UseProcessVariant>> {
    using ClockSourceType = std::chrono::system_clock;
    PortIn<T>                                in;
    std::vector<T>                           samples{};
    std::vector<Tag>                         tags{};
    std::uint64_t                            n_samples_expected{ 0 };
    std::uint64_t                            n_samples_produced{ 0 };
    float                                    sample_rate = 1000.0f;
    std::string                              signal_name;
    bool                                     log_tags         = true;
    bool                                     log_samples      = true;
    bool                                     verbose_console  = false;
    std::chrono::time_point<ClockSourceType> _timeFirstSample = ClockSourceType::now();
    std::chrono::time_point<ClockSourceType> _timeLastSample  = ClockSourceType::now();

    void
    start() {
        if (verbose_console) {
            fmt::println("started sink {} aka. '{}'", this->unique_name, this->name);
        }
        _timeFirstSample = ClockSourceType::now();
        samples.clear();
        if (log_samples) {
            samples.reserve(std::max(0UZ, static_cast<std::size_t>(n_samples_expected)));
        }
        tags.clear();
    }

    void
    stop() {
        if (verbose_console) {
            fmt::println("stopped sink {} aka. '{}'", this->unique_name, this->name);
        }
    }

    constexpr void
    processOne(const T &input) noexcept // N.B. non-SIMD since we need a sample-by-sample accurate tag detection
        requires(UseProcessVariant == ProcessFunction::USE_PROCESS_ONE)
    {
        if (this->input_tags_present()) {
            const Tag &tag = this->mergedInputTag();
            if (verbose_console) {
                print_tag(tag, fmt::format("{}::processOne(...1)    \t received tag at {:6}", this->name, n_samples_produced));
            }
            if (log_tags) {
                tags.emplace_back(n_samples_produced, tag.map);
            }
        }
        if (log_samples) {
            samples.emplace_back(input);
        }
        n_samples_produced++;
        if (n_samples_expected > 0 && n_samples_produced >= n_samples_expected) {
            this->state = lifecycle::State::STOPPED;
        }
        _timeLastSample = ClockSourceType::now();
    }

    // template<gr::meta::t_or_simd<T> V>
    constexpr work::Status
    processBulk(std::span<const T> input) noexcept
        requires(UseProcessVariant == ProcessFunction::USE_PROCESS_BULK)
    {
        if (this->input_tags_present()) {
            const Tag &tag = this->mergedInputTag();
            if (verbose_console) {
                print_tag(tag, fmt::format("{}::processBulk(...{})\t received tag at {:6}", this->name, input.size(), n_samples_produced));
            }
            if (log_tags) {
                tags.emplace_back(n_samples_produced, tag.map);
            }
        }
        if (log_samples) {
            samples.insert(samples.cend(), input.begin(), input.end());
        }
        n_samples_produced += static_cast<std::uint64_t>(input.size());
        _timeLastSample = ClockSourceType::now();
        return n_samples_expected > 0 && n_samples_produced >= n_samples_expected ? work::Status::DONE : work::Status::OK;
    }

    float
    effective_sample_rate() const {
        const auto total_elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(_timeLastSample - _timeFirstSample).count();
        return total_elapsed_time == 0 ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(n_samples_produced) * 1e6f / static_cast<float>(total_elapsed_time);
    }
};

} // namespace gr::testing

ENABLE_REFLECTION_FOR_TEMPLATE_FULL((typename T, gr::testing::ProcessFunction b), (gr::testing::TagSource<T, b>), out, n_samples_max, sample_rate, signal_name, verbose_console, mark_tag);
ENABLE_REFLECTION_FOR_TEMPLATE_FULL((typename T, gr::testing::ProcessFunction b), (gr::testing::TagMonitor<T, b>), in, out, n_samples_expected, sample_rate, signal_name, n_samples_produced, log_tags,
                                    log_samples, verbose_console, samples);
ENABLE_REFLECTION_FOR_TEMPLATE_FULL((typename T, gr::testing::ProcessFunction b), (gr::testing::TagSink<T, b>), in, n_samples_expected, sample_rate, signal_name, n_samples_produced, log_tags,
                                    log_samples, verbose_console, samples);

namespace gr::testing {
// the concepts can only work as expected after ENABLE_REFLECTION_FOR_TEMPLATE_FULL
static_assert(HasProcessOneFunction<TagSource<int, ProcessFunction::USE_PROCESS_ONE>>);
static_assert(not HasProcessBulkFunction<TagSource<int, ProcessFunction::USE_PROCESS_ONE>>);
static_assert(HasRequiredProcessFunction<TagSource<int, ProcessFunction::USE_PROCESS_ONE>>);
static_assert(not HasProcessOneFunction<TagSource<int, ProcessFunction::USE_PROCESS_BULK>>);
static_assert(HasProcessBulkFunction<TagSource<int, ProcessFunction::USE_PROCESS_BULK>>);
static_assert(HasRequiredProcessFunction<TagSource<int, ProcessFunction::USE_PROCESS_BULK>>);

static_assert(HasProcessOneFunction<TagMonitor<int, ProcessFunction::USE_PROCESS_ONE>>);
static_assert(not HasProcessOneFunction<TagMonitor<int, ProcessFunction::USE_PROCESS_BULK>>);
static_assert(not HasProcessBulkFunction<TagMonitor<int, ProcessFunction::USE_PROCESS_ONE>>);
static_assert(not HasProcessBulkFunction<TagMonitor<int, ProcessFunction::USE_PROCESS_ONE_SIMD>>);
static_assert(HasProcessBulkFunction<TagMonitor<int, ProcessFunction::USE_PROCESS_BULK>>);
static_assert(HasRequiredProcessFunction<TagMonitor<int, ProcessFunction::USE_PROCESS_ONE>>);
static_assert(HasRequiredProcessFunction<TagMonitor<int, ProcessFunction::USE_PROCESS_ONE_SIMD>>);
static_assert(HasRequiredProcessFunction<TagMonitor<int, ProcessFunction::USE_PROCESS_BULK>>);

static_assert(HasRequiredProcessFunction<TagSink<int, ProcessFunction::USE_PROCESS_ONE>>);
static_assert(HasRequiredProcessFunction<TagSink<int, ProcessFunction::USE_PROCESS_BULK>>);
} // namespace gr::testing

#endif // GNURADIO_TAGMONITORS_HPP