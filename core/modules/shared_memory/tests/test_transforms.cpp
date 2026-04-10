#include <gtest/gtest.h>

#include "../../../utils/configs.hpp"
#include "../transforms.hpp"
#include "../utils.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class TrackingTransform : public core::Transform {
public:
    explicit TrackingTransform(bool& was_called_ref) : core::Transform(2, 2), _was_called_ref(was_called_ref) {}

    void apply(core::DataEntry& entry) override {
        _was_called_ref = true;
        entry.key = "wrapped";
    }

private:
    bool& _was_called_ref;
};

core::TransformConfig make_uyvy_config() {
    core::TransformConfig config;
    config.name = "UYVY2RGB";
    return config;
}

} // namespace

TEST(TransformsTest, UYVY2RgbInvalidInputSizeLeavesEntryUnchangedWithoutWrappedTransform) {
    // GIVEN: A UYVY2RGB transform has an input whose payload is too small
    core::DataEntry entry{
        .key = "camera",
        .data = {1U, 2U, 3U},
        .head = 5U,
        .sequence = 7U,
        .timestamp_ns = 11U,
    };

    const core::DataEntry original_entry = entry;
    core::UYVY2RGB transform(2, 2, make_uyvy_config(), nullptr);

    // WHEN: The transform is applied
    transform.apply(entry);

    // THEN: The entry is left unchanged
    EXPECT_EQ(entry.key, original_entry.key);
    EXPECT_EQ(entry.data, original_entry.data);
    EXPECT_EQ(entry.head, original_entry.head);
    EXPECT_EQ(entry.sequence, original_entry.sequence);
    EXPECT_EQ(entry.timestamp_ns, original_entry.timestamp_ns);
}

TEST(TransformsTest, UYVY2RgbAppliesWrappedTransformBeforeValidatingInputSize) {
    // GIVEN: A UYVY2RGB transform wraps another transform
    bool wrapped_transform_called = false;
    core::DataEntry entry{
        .key = "camera",
        .data = {9U, 8U, 7U},
        .head = 0U,
        .sequence = 1U,
        .timestamp_ns = 2U,
    };

    auto wrapped_transform = std::make_unique<TrackingTransform>(wrapped_transform_called);
    core::UYVY2RGB transform(2, 2, make_uyvy_config(), std::move(wrapped_transform));

    // WHEN: The transform is applied to an input with an invalid payload size
    transform.apply(entry);

    // THEN: The wrapped transform runs before input-size validation stops conversion
    EXPECT_TRUE(wrapped_transform_called);
    EXPECT_EQ(entry.key, "wrapped");
    EXPECT_EQ(entry.data, std::vector<uint8_t>({9U, 8U, 7U}));
}
