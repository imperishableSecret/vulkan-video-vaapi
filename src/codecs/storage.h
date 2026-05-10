#ifndef VKVV_CODECS_STORAGE_H
#define VKVV_CODECS_STORAGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vkvv {

    template <typename T>
    void clear_with_capacity_hysteresis(std::vector<T>& values, uint32_t& underused_frames) {
        constexpr size_t   shrink_capacity_ratio = 4;
        constexpr size_t   minimum_capacity      = 64;
        constexpr uint32_t shrink_frame_count    = 32;

        const size_t       observed_size = values.size();
        if (values.capacity() >= minimum_capacity && (observed_size == 0 || values.capacity() / observed_size >= shrink_capacity_ratio)) {
            underused_frames++;
            if (underused_frames >= shrink_frame_count) {
                std::vector<T>().swap(values);
                underused_frames = 0;
                return;
            }
        } else {
            underused_frames = 0;
        }
        values.clear();
    }

} // namespace vkvv

#endif
