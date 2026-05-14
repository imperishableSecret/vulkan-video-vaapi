#include "av1.h"
#include "codecs/storage.h"
#include "telemetry.h"

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif
#include <gst/codecparsers/gstav1parser.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {

    struct AV1State {
        ~AV1State() {
            if (parser != nullptr) {
                gst_av1_parser_free(parser);
            }
        }

        bool                                   has_pic            = false;
        bool                                   has_slice_data     = false;
        bool                                   has_header         = false;
        bool                                   has_sequence       = false;
        bool                                   tile_records_valid = true;

        GstAV1Parser*                          parser = nullptr;
        VADecPictureParameterBufferAV1         pic{};
        std::vector<VASliceParameterBufferAV1> pending_slices;
        std::vector<VkvvAV1Tile>               tiles;
        std::vector<uint8_t>                   bitstream;
        std::vector<VkvvAV1Tile>               decode_tiles;
        uint32_t                               decode_window_offset = 0;
        size_t                                 decode_window_size   = 0;
        VkvvAV1FrameHeader                     header{};
        VkvvAV1SequenceHeader                  sequence{};
        uint32_t                               pending_slices_underused_frames = 0;
        uint32_t                               tiles_underused_frames          = 0;
        uint32_t                               bitstream_underused_frames      = 0;
        uint32_t                               decode_tiles_underused_frames   = 0;
    };

    uint8_t av1_bit_depth(const VADecPictureParameterBufferAV1& pic) {
        switch (pic.bit_depth_idx) {
            case 0: return 8;
            case 1: return 10;
            case 2: return 12;
            default: return 0;
        }
    }

    bool copy_first_element(const VkvvBuffer* buffer, void* dst, size_t dst_size) {
        if (buffer == nullptr || buffer->data == nullptr || buffer->size < dst_size || buffer->num_elements == 0) {
            return false;
        }
        std::memcpy(dst, buffer->data, dst_size);
        return true;
    }

    bool copy_slice_elements(AV1State* av1, const VkvvBuffer* buffer) {
        if (av1 == nullptr || buffer == nullptr || buffer->data == nullptr || buffer->size < sizeof(VASliceParameterBufferAV1) || buffer->num_elements == 0) {
            return false;
        }

        const auto* params = static_cast<const VASliceParameterBufferAV1*>(buffer->data);
        av1->pending_slices.insert(av1->pending_slices.end(), params, params + buffer->num_elements);
        return true;
    }

    uint32_t tile_index_from_param(const VADecPictureParameterBufferAV1& pic, const VASliceParameterBufferAV1& param, uint32_t fallback) {
        if (param.tile_row < pic.tile_rows && param.tile_column < pic.tile_cols) {
            return static_cast<uint32_t>(param.tile_row) * pic.tile_cols + param.tile_column;
        }
        return fallback;
    }

    uint32_t expected_tile_count(const VADecPictureParameterBufferAV1& pic) {
        return static_cast<uint32_t>(pic.tile_cols) * static_cast<uint32_t>(pic.tile_rows);
    }

    bool record_tile_offsets(AV1State* av1, int64_t offset_adjustment) {
        if (av1 == nullptr || !av1->has_pic) {
            return false;
        }

        const uint64_t total_size = av1->bitstream.size();
        for (const VASliceParameterBufferAV1& param : av1->pending_slices) {
            if (param.slice_data_size == 0) {
                av1->tile_records_valid = false;
                return false;
            }
            const int64_t adjusted_offset = offset_adjustment + param.slice_data_offset;
            if (adjusted_offset < 0 || static_cast<uint64_t>(adjusted_offset) > std::numeric_limits<uint32_t>::max() ||
                param.slice_data_size > std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(adjusted_offset)) {
                av1->tile_records_valid = false;
                return false;
            }
            const uint64_t offset = static_cast<uint64_t>(adjusted_offset);
            const uint64_t end    = offset + param.slice_data_size;
            if (end > total_size) {
                av1->tile_records_valid = false;
                return false;
            }

            VkvvAV1Tile tile{};
            tile.param      = param;
            tile.tile_index = tile_index_from_param(av1->pic, param, static_cast<uint32_t>(av1->tiles.size()));
            tile.offset     = static_cast<uint32_t>(offset);
            tile.size       = param.slice_data_size;
            av1->tiles.push_back(tile);
        }
        av1->pending_slices.clear();
        return true;
    }

    bool obu_payload_offset(const uint8_t* base, size_t buffer_size, const GstAV1OBU& obu, uint32_t* payload_offset) {
        if (base == nullptr || obu.data == nullptr || payload_offset == nullptr || obu.data < base || obu.data > base + buffer_size) {
            return false;
        }

        const size_t offset = static_cast<size_t>(obu.data - base);
        if (offset > buffer_size || obu.obu_size > buffer_size - offset || offset > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        *payload_offset = static_cast<uint32_t>(offset);
        return true;
    }

    bool submitted_tile_param_for_range(const AV1State* av1, uint32_t offset, uint32_t size, VASliceParameterBufferAV1* param) {
        if (av1 == nullptr || param == nullptr || size == 0) {
            return false;
        }

        const uint64_t end = static_cast<uint64_t>(offset) + size;
        for (const VkvvAV1Tile& submitted : av1->tiles) {
            const uint64_t submitted_end = static_cast<uint64_t>(submitted.offset) + submitted.size;
            if (submitted.offset <= offset && end <= submitted_end) {
                *param = submitted.param;
                return true;
            }
        }
        return false;
    }

    bool submitted_ranges_intersect(const AV1State* av1, uint32_t offset, uint32_t size) {
        if (av1 == nullptr || size == 0) {
            return false;
        }

        const uint64_t end = static_cast<uint64_t>(offset) + size;
        for (const VkvvAV1Tile& submitted : av1->tiles) {
            const uint64_t submitted_end = static_cast<uint64_t>(submitted.offset) + submitted.size;
            if (offset < submitted_end && submitted.offset < end) {
                return true;
            }
        }
        return false;
    }

    bool append_tile_group_tiles(const AV1State* av1, const GstAV1OBU& obu, const GstAV1TileGroupOBU& group, std::vector<VkvvAV1Tile>* tiles, char* reason, size_t reason_size) {
        if (av1 == nullptr || tiles == nullptr || !av1->has_pic) {
            std::snprintf(reason, reason_size, "missing AV1 tile-group state");
            return false;
        }

        const uint32_t total_tiles = expected_tile_count(av1->pic);
        if (total_tiles == 0 || total_tiles > VKVV_AV1_MAX_TILES) {
            std::snprintf(reason, reason_size, "invalid AV1 tile layout: cols=%u rows=%u", av1->pic.tile_cols, av1->pic.tile_rows);
            return false;
        }
        if (group.num_tiles != 0 && group.num_tiles != total_tiles) {
            std::snprintf(reason, reason_size, "AV1 tile-group layout mismatch: expected=%u parsed=%u", total_tiles, group.num_tiles);
            return false;
        }
        if (group.tg_end < group.tg_start || group.tg_end >= total_tiles) {
            std::snprintf(reason, reason_size, "invalid AV1 tile-group range: start=%u end=%u total=%u", group.tg_start, group.tg_end, total_tiles);
            return false;
        }

        uint32_t payload_offset = 0;
        if (!obu_payload_offset(av1->bitstream.data(), av1->bitstream.size(), obu, &payload_offset)) {
            std::snprintf(reason, reason_size, "invalid AV1 tile-group payload bounds");
            return false;
        }

        for (uint32_t i = group.tg_start; i <= group.tg_end; i++) {
            const GstAV1TileGroupOBUEntry& entry = group.entry[i];
            if (entry.tile_row >= av1->pic.tile_rows || entry.tile_col >= av1->pic.tile_cols) {
                std::snprintf(reason, reason_size, "AV1 tile coordinates exceed picture layout: tile=%u row=%u col=%u rows=%u cols=%u", i, entry.tile_row, entry.tile_col,
                              av1->pic.tile_rows, av1->pic.tile_cols);
                return false;
            }
            const uint32_t tile_index = entry.tile_row * static_cast<uint32_t>(av1->pic.tile_cols) + entry.tile_col;
            if (tile_index != i) {
                std::snprintf(reason, reason_size, "AV1 tile-group index mismatch: expected=%u row=%u col=%u got=%u", i, entry.tile_row, entry.tile_col, tile_index);
                return false;
            }
            if (entry.tile_size == 0 || entry.tile_offset > obu.obu_size || entry.tile_size > obu.obu_size - entry.tile_offset) {
                std::snprintf(reason, reason_size, "AV1 tile exceeds tile-group OBU: tile=%u offset=%u size=%u obu=%u", i, entry.tile_offset, entry.tile_size, obu.obu_size);
                return false;
            }

            const uint64_t absolute_offset = static_cast<uint64_t>(payload_offset) + entry.tile_offset;
            const uint64_t absolute_end    = absolute_offset + entry.tile_size;
            if (absolute_offset > std::numeric_limits<uint32_t>::max() || absolute_end > av1->bitstream.size()) {
                std::snprintf(reason, reason_size, "AV1 tile exceeds submitted bitstream: tile=%u offset=%llu size=%u bytes=%zu", i,
                              static_cast<unsigned long long>(absolute_offset), entry.tile_size, av1->bitstream.size());
                return false;
            }

            VASliceParameterBufferAV1 param{};
            if (!submitted_tile_param_for_range(av1, static_cast<uint32_t>(absolute_offset), entry.tile_size, &param)) {
                std::snprintf(reason, reason_size, "AV1 parsed tile is outside submitted tile ranges: tile=%u offset=%llu size=%u", i,
                              static_cast<unsigned long long>(absolute_offset), entry.tile_size);
                return false;
            }

            param.slice_data_offset = static_cast<uint32_t>(absolute_offset);
            param.slice_data_size   = entry.tile_size;
            param.tile_row          = static_cast<uint16_t>(entry.tile_row);
            param.tile_column       = static_cast<uint16_t>(entry.tile_col);

            VkvvAV1Tile tile{};
            tile.param      = param;
            tile.tile_index = tile_index;
            tile.offset     = param.slice_data_offset;
            tile.size       = param.slice_data_size;
            tiles->push_back(tile);
        }
        return true;
    }

    bool min_tile_offset(const AV1State* av1, uint32_t* offset) {
        if (av1 == nullptr || offset == nullptr || av1->tiles.empty()) {
            return false;
        }

        uint32_t min_offset = std::numeric_limits<uint32_t>::max();
        for (const VkvvAV1Tile& tile : av1->tiles) {
            min_offset = std::min(min_offset, tile.offset);
        }
        *offset = min_offset;
        return true;
    }

    bool av1_decode_window(AV1State* av1, uint32_t* window_offset, char* reason, size_t reason_size) {
        if (av1 == nullptr || av1->bitstream.empty() || av1->tiles.empty() || !av1->header.valid) {
            std::snprintf(reason, reason_size, "missing AV1 decode window input");
            return false;
        }

        uint32_t first_tile_offset = 0;
        if (!min_tile_offset(av1, &first_tile_offset)) {
            std::snprintf(reason, reason_size, "missing AV1 tile offsets");
            return false;
        }

        const uint32_t start = av1->header.frame_header_offset <= first_tile_offset ? av1->header.frame_header_offset : 0;
        uint64_t       end   = start;
        for (const VkvvAV1Tile& tile : av1->tiles) {
            if (tile.offset < start) {
                std::snprintf(reason, reason_size, "AV1 tile offset precedes frame window: tile=%u window=%u", tile.offset, start);
                return false;
            }
            const uint64_t tile_end = static_cast<uint64_t>(tile.offset) + tile.size;
            if (tile.size == 0 || tile_end > av1->bitstream.size() || tile_end > std::numeric_limits<uint32_t>::max()) {
                std::snprintf(reason, reason_size, "AV1 tile exceeds bitstream: offset=%u size=%u bytes=%zu", tile.offset, tile.size, av1->bitstream.size());
                return false;
            }
            end = std::max(end, tile_end);
        }
        if (end <= start || end > av1->bitstream.size()) {
            std::snprintf(reason, reason_size, "invalid AV1 decode window: start=%u end=%llu bytes=%zu", start, static_cast<unsigned long long>(end), av1->bitstream.size());
            return false;
        }

        av1->decode_tiles = av1->tiles;
        for (VkvvAV1Tile& tile : av1->decode_tiles) {
            tile.offset -= start;
        }
        av1->decode_window_offset = start;
        av1->decode_window_size   = static_cast<size_t>(end - start);
        av1->header.frame_header_offset -= start;
        if (window_offset != nullptr) {
            *window_offset = start;
        }
        return true;
    }

    bool obu_contains_offset(const uint8_t* base, size_t buffer_size, const GstAV1OBU& obu, uint32_t obu_start, uint32_t consumed, uint32_t target_offset) {
        const size_t obu_end       = std::min<size_t>(static_cast<size_t>(obu_start) + consumed, buffer_size);
        size_t       payload_start = obu_start;
        size_t       payload_end   = obu_end;
        if (obu.data != nullptr && base != nullptr && obu.data >= base && obu.data <= base + buffer_size) {
            payload_start = static_cast<size_t>(obu.data - base);
            payload_end   = std::min<size_t>(payload_start + obu.obu_size, buffer_size);
        }

        const size_t target = target_offset;
        if (target >= payload_start && target < payload_end) {
            return true;
        }
        return target >= obu_start && target < obu_end;
    }

    void copy_gst_frame_header(const GstAV1FrameHeaderOBU& src, uint32_t frame_header_offset, VkvvAV1FrameHeader* dst) {
        *dst                                  = {};
        dst->valid                            = true;
        dst->show_existing_frame              = src.show_existing_frame;
        dst->show_frame                       = src.show_frame;
        dst->showable_frame                   = src.showable_frame;
        dst->error_resilient_mode             = src.error_resilient_mode;
        dst->disable_cdf_update               = src.disable_cdf_update;
        dst->frame_size_override_flag         = src.frame_size_override_flag;
        dst->buffer_removal_time_present_flag = src.buffer_removal_time_present_flag;
        dst->allow_intrabc                    = src.allow_intrabc;
        dst->frame_refs_short_signaling       = src.frame_refs_short_signaling;
        dst->allow_high_precision_mv          = src.allow_high_precision_mv;
        dst->is_motion_mode_switchable        = src.is_motion_mode_switchable;
        dst->use_ref_frame_mvs                = src.use_ref_frame_mvs;
        dst->disable_frame_end_update_cdf     = src.disable_frame_end_update_cdf;
        dst->allow_warped_motion              = src.allow_warped_motion;
        dst->reduced_tx_set                   = src.reduced_tx_set;
        dst->render_and_frame_size_different  = src.render_and_frame_size_different;
        dst->use_superres                     = src.use_superres;
        dst->is_filter_switchable             = src.is_filter_switchable;
        dst->skip_mode_present                = src.skip_mode_present;
        dst->reference_select                 = src.reference_select;
        dst->frame_type                       = static_cast<uint8_t>(src.frame_type);
        dst->primary_ref_frame                = src.primary_ref_frame;
        dst->refresh_frame_flags              = src.refresh_frame_flags;
        dst->allow_screen_content_tools       = src.allow_screen_content_tools;
        dst->force_integer_mv                 = src.force_integer_mv;
        dst->interpolation_filter             = static_cast<uint8_t>(src.interpolation_filter);
        dst->tx_mode                          = static_cast<uint8_t>(src.tx_mode);
        dst->tile_size_bytes_minus_1          = src.tile_info.tile_size_bytes_minus_1;
        dst->frame_to_show_map_idx            = src.frame_to_show_map_idx;
        dst->current_frame_id                 = src.current_frame_id;
        dst->display_frame_id                 = src.display_frame_id;
        dst->order_hint                       = src.order_hint;
        dst->frame_header_offset              = frame_header_offset;
        std::copy(std::begin(src.skip_mode_frame), std::end(src.skip_mode_frame), dst->skip_mode_frame);
        std::copy(std::begin(src.ref_frame_idx), std::end(src.ref_frame_idx), dst->ref_frame_idx);
        std::copy(std::begin(src.order_hints), std::end(src.order_hints), dst->order_hints);
        std::copy(std::begin(src.ref_frame_sign_bias), std::end(src.ref_frame_sign_bias), dst->ref_frame_sign_bias);
        std::copy(std::begin(src.expected_frame_id), std::end(src.expected_frame_id), dst->expected_frame_id);
    }

    void copy_gst_sequence_header(const GstAV1SequenceHeaderOBU& src, VkvvAV1SequenceHeader* dst) {
        *dst                                    = {};
        dst->valid                              = true;
        dst->still_picture                      = src.still_picture;
        dst->reduced_still_picture_header       = src.reduced_still_picture_header;
        dst->frame_id_numbers_present_flag      = src.frame_id_numbers_present_flag;
        dst->use_128x128_superblock             = src.use_128x128_superblock;
        dst->enable_filter_intra                = src.enable_filter_intra;
        dst->enable_intra_edge_filter           = src.enable_intra_edge_filter;
        dst->enable_interintra_compound         = src.enable_interintra_compound;
        dst->enable_masked_compound             = src.enable_masked_compound;
        dst->enable_warped_motion               = src.enable_warped_motion;
        dst->enable_order_hint                  = src.enable_order_hint;
        dst->enable_dual_filter                 = src.enable_dual_filter;
        dst->enable_jnt_comp                    = src.enable_jnt_comp;
        dst->enable_ref_frame_mvs               = src.enable_ref_frame_mvs;
        dst->enable_superres                    = src.enable_superres;
        dst->enable_cdef                        = src.enable_cdef;
        dst->enable_restoration                 = src.enable_restoration;
        dst->film_grain_params_present          = src.film_grain_params_present;
        dst->timing_info_present_flag           = src.timing_info_present_flag;
        dst->equal_picture_interval             = src.timing_info.equal_picture_interval;
        dst->initial_display_delay_present_flag = src.initial_display_delay_present_flag;
        dst->mono_chrome                        = src.color_config.mono_chrome;
        dst->color_range                        = src.color_config.color_range;
        dst->separate_uv_delta_q                = src.color_config.separate_uv_delta_q;
        dst->color_description_present_flag     = src.color_config.color_description_present_flag;
        dst->seq_profile                        = static_cast<uint8_t>(src.seq_profile);
        dst->bit_depth                          = src.bit_depth;
        dst->frame_width_bits_minus_1           = src.frame_width_bits_minus_1;
        dst->frame_height_bits_minus_1          = src.frame_height_bits_minus_1;
        dst->max_frame_width_minus_1            = src.max_frame_width_minus_1;
        dst->max_frame_height_minus_1           = src.max_frame_height_minus_1;
        dst->delta_frame_id_length_minus_2      = src.delta_frame_id_length_minus_2;
        dst->additional_frame_id_length_minus_1 = src.additional_frame_id_length_minus_1;
        dst->order_hint_bits_minus_1            = src.order_hint_bits_minus_1;
        dst->seq_force_integer_mv               = src.seq_choose_integer_mv ? GST_AV1_SELECT_INTEGER_MV : src.seq_force_integer_mv;
        dst->seq_force_screen_content_tools     = src.seq_choose_screen_content_tools ? GST_AV1_SELECT_SCREEN_CONTENT_TOOLS : src.seq_force_screen_content_tools;
        dst->subsampling_x                      = src.color_config.subsampling_x;
        dst->subsampling_y                      = src.color_config.subsampling_y;
        dst->chroma_sample_position             = static_cast<uint8_t>(src.color_config.chroma_sample_position);
        dst->color_primaries                    = static_cast<uint8_t>(src.color_config.color_primaries);
        dst->transfer_characteristics           = static_cast<uint8_t>(src.color_config.transfer_characteristics);
        dst->matrix_coefficients                = static_cast<uint8_t>(src.color_config.matrix_coefficients);
        dst->num_units_in_display_tick          = src.timing_info.num_units_in_display_tick;
        dst->time_scale                         = src.timing_info.time_scale;
        dst->num_ticks_per_picture_minus_1      = src.timing_info.num_ticks_per_picture_minus_1;
    }

    bool parse_av1_frame_header(AV1State* av1, char* reason, size_t reason_size) {
        if (av1 == nullptr || av1->parser == nullptr || av1->bitstream.empty()) {
            std::snprintf(reason, reason_size, "missing AV1 parser input");
            return false;
        }

        uint32_t                 offset             = 0;
        uint32_t                 target_tile_offset = 0;
        const bool               have_target_tile   = min_tile_offset(av1, &target_tile_offset);
        const uint32_t           expected_tiles     = av1->has_pic ? expected_tile_count(av1->pic) : 0;
        GstAV1FrameHeaderOBU     pending_header{};
        uint32_t                 pending_header_offset       = 0;
        bool                     have_pending_header         = false;
        bool                     pending_tiles_authoritative = false;
        std::vector<VkvvAV1Tile> pending_tiles;
        GstAV1OBU                pending_header_obu{};
        uint32_t                 pending_header_obu_offset                  = 0;
        bool                     have_pending_header_obu                    = false;
        bool                     pending_header_obu_updates_reference_state = false;
        GstAV1FrameHeaderOBU     selected_header{};
        uint32_t                 selected_header_offset           = 0;
        bool                     have_selected_header             = false;
        bool                     selected_updates_reference_state = false;
        bool                     selected_tiles_authoritative     = false;
        std::vector<VkvvAV1Tile> selected_tiles;
        GstAV1FrameHeaderOBU     fallback_header{};
        uint32_t                 fallback_header_offset           = 0;
        bool                     have_fallback_header             = false;
        bool                     fallback_updates_reference_state = false;
        bool                     fallback_tiles_authoritative     = false;
        std::vector<VkvvAV1Tile> fallback_tiles;

        auto                     selected_has_required_tiles = [&]() {
            return selected_header.show_existing_frame || (selected_tiles_authoritative && expected_tiles != 0 && selected_tiles.size() >= expected_tiles);
        };

        auto parse_pending_header_obu = [&]() {
            if (have_pending_header) {
                return true;
            }
            if (!have_pending_header_obu) {
                std::snprintf(reason, reason_size, "AV1 tile group did not have a preceding frame header");
                return false;
            }

            GstAV1FrameHeaderOBU     frame_header{};
            GstAV1OBU                obu_copy     = pending_header_obu;
            const GstAV1ParserResult parse_result = gst_av1_parser_parse_frame_header_obu(av1->parser, &obu_copy, &frame_header);
            if (parse_result != GST_AV1_PARSER_OK) {
                std::snprintf(reason, reason_size, "failed to parse selected AV1 frame header: parser_result=%d offset=%u", parse_result, pending_header_obu_offset);
                return false;
            }

            pending_header              = frame_header;
            pending_header_offset       = pending_header_obu_offset;
            have_pending_header         = true;
            pending_tiles_authoritative = false;
            pending_tiles.clear();
            have_fallback_header             = true;
            fallback_header                  = frame_header;
            fallback_header_offset           = pending_header_obu_offset;
            fallback_updates_reference_state = pending_header_obu_updates_reference_state;
            fallback_tiles.clear();
            fallback_tiles_authoritative = false;
            return true;
        };

        while (offset < av1->bitstream.size()) {
            GstAV1OBU                obu{};
            uint32_t                 consumed  = 0;
            const uint32_t           obu_start = offset;
            const GstAV1ParserResult identify_result =
                gst_av1_parser_identify_one_obu(av1->parser, av1->bitstream.data() + offset, static_cast<uint32_t>(av1->bitstream.size() - offset), &obu, &consumed);
            if (identify_result != GST_AV1_PARSER_OK || consumed == 0) {
                std::snprintf(reason, reason_size, "failed to identify AV1 OBU: parser_result=%d offset=%u bytes=%zu first=%02x%02x%02x%02x", identify_result, offset,
                              av1->bitstream.size(), av1->bitstream.size() > 0 ? av1->bitstream[0] : 0, av1->bitstream.size() > 1 ? av1->bitstream[1] : 0,
                              av1->bitstream.size() > 2 ? av1->bitstream[2] : 0, av1->bitstream.size() > 3 ? av1->bitstream[3] : 0);
                return false;
            }
            if (have_selected_header && !selected_has_required_tiles() &&
                (obu.obu_type == GST_AV1_OBU_FRAME_HEADER || obu.obu_type == GST_AV1_OBU_REDUNDANT_FRAME_HEADER || obu.obu_type == GST_AV1_OBU_FRAME)) {
                break;
            }

            if (obu.obu_type == GST_AV1_OBU_SEQUENCE_HEADER) {
                GstAV1SequenceHeaderOBU  sequence{};
                const GstAV1ParserResult parse_result = gst_av1_parser_parse_sequence_header_obu(av1->parser, &obu, &sequence);
                if (parse_result != GST_AV1_PARSER_OK) {
                    std::snprintf(reason, reason_size, "failed to parse AV1 sequence header: parser_result=%d offset=%u", parse_result, offset);
                    return false;
                }
                copy_gst_sequence_header(sequence, &av1->sequence);
                av1->has_sequence = true;
            } else if (obu.obu_type == GST_AV1_OBU_TEMPORAL_DELIMITER) {
                const GstAV1ParserResult parse_result = gst_av1_parser_parse_temporal_delimiter_obu(av1->parser, &obu);
                if (parse_result != GST_AV1_PARSER_OK) {
                    std::snprintf(reason, reason_size, "failed to parse AV1 temporal delimiter: parser_result=%d offset=%u", parse_result, offset);
                    return false;
                }
            } else if (obu.obu_type == GST_AV1_OBU_FRAME_HEADER || obu.obu_type == GST_AV1_OBU_REDUNDANT_FRAME_HEADER) {
                const bool header_matches_target =
                    have_target_tile && obu_contains_offset(av1->bitstream.data(), av1->bitstream.size(), obu, obu_start, consumed, target_tile_offset);
                if (have_target_tile && !header_matches_target) {
                    if (obu.obu_type != GST_AV1_OBU_REDUNDANT_FRAME_HEADER) {
                        pending_header_obu                         = obu;
                        pending_header_obu_offset                  = offset;
                        have_pending_header_obu                    = true;
                        pending_header_obu_updates_reference_state = true;
                        have_pending_header                        = false;
                        pending_tiles_authoritative                = false;
                        pending_tiles.clear();
                    }
                    offset += consumed;
                    continue;
                }

                GstAV1FrameHeaderOBU     frame_header{};
                const GstAV1ParserResult parse_result = gst_av1_parser_parse_frame_header_obu(av1->parser, &obu, &frame_header);
                if (parse_result != GST_AV1_PARSER_OK) {
                    std::snprintf(reason, reason_size, "failed to parse AV1 frame header: parser_result=%d offset=%u", parse_result, offset);
                    return false;
                }
                have_fallback_header             = true;
                fallback_header                  = frame_header;
                fallback_header_offset           = offset;
                fallback_updates_reference_state = obu.obu_type != GST_AV1_OBU_REDUNDANT_FRAME_HEADER;
                fallback_tiles.clear();
                fallback_tiles_authoritative = false;
                if (obu.obu_type != GST_AV1_OBU_REDUNDANT_FRAME_HEADER) {
                    pending_header              = frame_header;
                    pending_header_offset       = offset;
                    have_pending_header         = true;
                    have_pending_header_obu     = false;
                    pending_tiles_authoritative = false;
                    pending_tiles.clear();
                }
                if (header_matches_target) {
                    selected_header                  = frame_header;
                    selected_header_offset           = offset;
                    selected_updates_reference_state = obu.obu_type != GST_AV1_OBU_REDUNDANT_FRAME_HEADER;
                    selected_tiles.clear();
                    selected_tiles_authoritative = false;
                    have_selected_header         = true;
                }
            } else if (obu.obu_type == GST_AV1_OBU_FRAME) {
                const bool frame_matches_target =
                    have_target_tile && obu_contains_offset(av1->bitstream.data(), av1->bitstream.size(), obu, obu_start, consumed, target_tile_offset);
                const bool frame_intersects_submitted = submitted_ranges_intersect(av1, obu_start, consumed);
                const bool frame_candidate            = !have_target_tile || frame_matches_target || frame_intersects_submitted;
                if (!frame_candidate) {
                    offset += consumed;
                    continue;
                }

                GstAV1FrameOBU           frame{};
                const GstAV1ParserResult parse_result = gst_av1_parser_parse_frame_obu(av1->parser, &obu, &frame);
                if (parse_result != GST_AV1_PARSER_OK) {
                    std::snprintf(reason, reason_size, "failed to parse AV1 frame OBU: parser_result=%d offset=%u", parse_result, offset);
                    return false;
                }
                std::vector<VkvvAV1Tile> frame_tiles;
                bool                     frame_tiles_authoritative = false;
                if (!frame.frame_header.show_existing_frame) {
                    if (!append_tile_group_tiles(av1, obu, frame.tile_group, &frame_tiles, reason, reason_size)) {
                        return false;
                    }
                    frame_tiles_authoritative = true;
                }
                have_fallback_header             = true;
                fallback_header                  = frame.frame_header;
                fallback_header_offset           = offset;
                fallback_updates_reference_state = true;
                fallback_tiles                   = frame_tiles;
                fallback_tiles_authoritative     = frame_tiles_authoritative;
                if (have_target_tile && (frame_matches_target || frame_intersects_submitted)) {
                    selected_header                  = frame.frame_header;
                    selected_header_offset           = offset;
                    selected_updates_reference_state = true;
                    selected_tiles                   = frame_tiles;
                    selected_tiles_authoritative     = frame_tiles_authoritative;
                    have_selected_header             = true;
                    if (selected_has_required_tiles()) {
                        break;
                    }
                }
            } else if (obu.obu_type == GST_AV1_OBU_TILE_GROUP) {
                const bool group_matches_target =
                    have_target_tile && obu_contains_offset(av1->bitstream.data(), av1->bitstream.size(), obu, obu_start, consumed, target_tile_offset);
                const bool group_intersects_submitted = submitted_ranges_intersect(av1, obu_start, consumed);
                const bool group_candidate =
                    !have_target_tile || group_matches_target || group_intersects_submitted || (have_selected_header && selected_header_offset == pending_header_offset);
                if (!group_candidate) {
                    offset += consumed;
                    continue;
                }
                if (!parse_pending_header_obu()) {
                    return false;
                }
                if (!pending_header.show_existing_frame) {
                    GstAV1TileGroupOBU       tile_group{};
                    const GstAV1ParserResult parse_result = gst_av1_parser_parse_tile_group_obu(av1->parser, &obu, &tile_group);
                    if (parse_result != GST_AV1_PARSER_OK) {
                        std::snprintf(reason, reason_size, "failed to parse AV1 tile group: parser_result=%d offset=%u", parse_result, offset);
                        return false;
                    }
                    if (!append_tile_group_tiles(av1, obu, tile_group, &pending_tiles, reason, reason_size)) {
                        return false;
                    }
                    pending_tiles_authoritative = true;
                }
                if (group_candidate) {
                    have_fallback_header             = true;
                    fallback_header                  = pending_header;
                    fallback_header_offset           = pending_header_offset;
                    fallback_updates_reference_state = true;
                    fallback_tiles                   = pending_tiles;
                    fallback_tiles_authoritative     = pending_tiles_authoritative;
                }
                if (have_target_tile && (group_matches_target || (!have_selected_header && group_intersects_submitted))) {
                    selected_header                  = pending_header;
                    selected_header_offset           = pending_header_offset;
                    selected_updates_reference_state = true;
                    selected_tiles                   = pending_tiles;
                    selected_tiles_authoritative     = pending_tiles_authoritative;
                    have_selected_header             = true;
                } else if (have_selected_header && selected_header_offset == pending_header_offset) {
                    selected_tiles               = pending_tiles;
                    selected_tiles_authoritative = pending_tiles_authoritative;
                }
                if (have_selected_header && selected_header_offset == pending_header_offset && selected_has_required_tiles()) {
                    break;
                }
            }

            offset += consumed;
        }

        if (!have_selected_header && !have_target_tile && have_fallback_header) {
            selected_header                  = fallback_header;
            selected_header_offset           = fallback_header_offset;
            selected_updates_reference_state = fallback_updates_reference_state;
            selected_tiles                   = fallback_tiles;
            selected_tiles_authoritative     = fallback_tiles_authoritative;
            have_selected_header             = true;
        }

        if (!have_selected_header) {
            std::snprintf(reason, reason_size, "AV1 bitstream did not contain a frame header OBU");
            return false;
        }
        if (!selected_header.show_existing_frame) {
            if (!selected_tiles_authoritative) {
                std::snprintf(reason, reason_size, "AV1 bitstream did not contain tile groups for the selected frame");
                return false;
            }
            av1->tiles = selected_tiles;
        }

        copy_gst_frame_header(selected_header, selected_header_offset, &av1->header);
        if (selected_updates_reference_state) {
            const GstAV1ParserResult update_result = gst_av1_parser_reference_frame_update(av1->parser, &selected_header);
            if (update_result != GST_AV1_PARSER_OK) {
                std::snprintf(reason, reason_size, "failed to update AV1 reference parser state: parser_result=%d offset=%u", update_result, selected_header_offset);
                return false;
            }
        }

        av1->has_header = true;
        return true;
    }

    bool sort_and_validate_tiles(AV1State* av1, char* reason, size_t reason_size) {
        if (av1 == nullptr) {
            std::snprintf(reason, reason_size, "missing AV1 state");
            return false;
        }
        if (!av1->tile_records_valid) {
            std::snprintf(reason, reason_size, "invalid AV1 tile offsets");
            return false;
        }

        const uint32_t expected_tile_count = static_cast<uint32_t>(av1->pic.tile_cols) * static_cast<uint32_t>(av1->pic.tile_rows);
        if (expected_tile_count == 0 || expected_tile_count > VKVV_AV1_MAX_TILES) {
            std::snprintf(reason, reason_size, "invalid AV1 tile layout: cols=%u rows=%u", av1->pic.tile_cols, av1->pic.tile_rows);
            return false;
        }
        if (av1->tiles.size() != expected_tile_count) {
            std::snprintf(reason, reason_size, "AV1 tile count mismatch: expected=%u got=%zu", expected_tile_count, av1->tiles.size());
            return false;
        }

        std::sort(av1->tiles.begin(), av1->tiles.end(), [](const VkvvAV1Tile& a, const VkvvAV1Tile& b) { return a.tile_index < b.tile_index; });
        for (uint32_t i = 0; i < expected_tile_count; i++) {
            if (av1->tiles[i].tile_index != i) {
                std::snprintf(reason, reason_size, "AV1 tile index gap: expected=%u got=%u", i, av1->tiles[i].tile_index);
                return false;
            }
        }
        return true;
    }

    bool av1_supported_bit_depth(uint8_t bit_depth) {
        return bit_depth == 8 || bit_depth == 10;
    }

    unsigned int av1_rt_format(uint8_t bit_depth) {
        return bit_depth > 8 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    }

    unsigned int av1_fourcc(uint8_t bit_depth) {
        return bit_depth > 8 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    }

} // namespace

void* vkvv_av1_state_create(void) {
    try {
        auto* state   = new AV1State();
        state->parser = gst_av1_parser_new();
        if (state->parser == nullptr) {
            delete state;
            return nullptr;
        }
        return state;
    } catch (const std::bad_alloc&) { return nullptr; }
}

void vkvv_av1_state_destroy(void* state) {
    delete static_cast<AV1State*>(state);
}

void vkvv_av1_begin_picture(void* state) {
    auto* av1 = static_cast<AV1State*>(state);
    if (av1 == nullptr) {
        return;
    }

    av1->has_pic            = false;
    av1->has_slice_data     = false;
    av1->has_header         = false;
    av1->tile_records_valid = true;
    av1->pic                = {};
    vkvv::clear_with_capacity_hysteresis(av1->pending_slices, av1->pending_slices_underused_frames);
    vkvv::clear_with_capacity_hysteresis(av1->tiles, av1->tiles_underused_frames);
    vkvv::clear_with_capacity_hysteresis(av1->bitstream, av1->bitstream_underused_frames);
    vkvv::clear_with_capacity_hysteresis(av1->decode_tiles, av1->decode_tiles_underused_frames);
    av1->decode_window_offset = 0;
    av1->decode_window_size   = 0;
    av1->header               = {};
}

VAStatus vkvv_av1_render_buffer(void* state, const VkvvBuffer* buffer) {
    auto* av1 = static_cast<AV1State*>(state);
    if (av1 == nullptr || buffer == nullptr) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    switch (buffer->type) {
        case VAPictureParameterBufferType:
            if (!copy_first_element(buffer, &av1->pic, sizeof(av1->pic))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            av1->has_pic = true;
            return VA_STATUS_SUCCESS;

        case VASliceParameterBufferType:
            if (!copy_slice_elements(av1, buffer)) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            if (!av1->bitstream.empty() && !record_tile_offsets(av1, 0)) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            return VA_STATUS_SUCCESS;

        case VASliceDataBufferType: {
            if (buffer->data == nullptr || buffer->num_elements == 0) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const size_t total_size = static_cast<size_t>(buffer->size) * buffer->num_elements;
            if (total_size == 0) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const size_t base = av1->bitstream.size();
            const auto*  data = static_cast<const uint8_t*>(buffer->data);
            av1->bitstream.insert(av1->bitstream.end(), data, data + total_size);
            av1->has_slice_data = !av1->bitstream.empty();
            if (!av1->pending_slices.empty() && !record_tile_offsets(av1, static_cast<int64_t>(base))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            return VA_STATUS_SUCCESS;
        }

        default: return VA_STATUS_SUCCESS;
    }
}

VAStatus vkvv_av1_prepare_decode(void* state, unsigned int* width, unsigned int* height, char* reason, size_t reason_size) {
    auto* av1 = static_cast<AV1State*>(state);
    if (av1 == nullptr) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_CONTEXT, "missing AV1 state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!av1->has_pic) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "missing AV1 picture parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!av1->has_slice_data) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "missing AV1 slice data");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (av1->pic.profile != 0) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "AV1 path currently supports only Profile0/Main");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    const uint8_t bit_depth = av1_bit_depth(av1->pic);
    if (!av1_supported_bit_depth(bit_depth)) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "AV1 path currently supports only 8-bit NV12 and 10-bit P010 decode");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (av1->pic.pic_info_fields.bits.large_scale_tile) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_UNIMPLEMENTED, "AV1 large-scale tile mode is not implemented");
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (av1->pic.film_grain_info.film_grain_info_fields.bits.apply_grain) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_UNIMPLEMENTED, "AV1 film grain output is not implemented yet");
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (av1->pic.frame_width_minus1 == std::numeric_limits<uint16_t>::max() || av1->pic.frame_height_minus1 == std::numeric_limits<uint16_t>::max()) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "AV1 picture dimensions are invalid");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!parse_av1_frame_header(av1, reason, reason_size)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!av1->has_sequence || !av1->sequence.valid) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "AV1 bitstream did not contain a sequence header before the frame");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (av1->sequence.seq_profile != av1->pic.profile || av1->sequence.bit_depth != bit_depth) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "AV1 sequence header does not match VA picture parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (av1->sequence.mono_chrome || !av1->sequence.subsampling_x || !av1->sequence.subsampling_y) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, "AV1 path currently supports only 4:2:0");
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    *width  = static_cast<unsigned int>(av1->pic.frame_width_minus1) + 1;
    *height = static_cast<unsigned int>(av1->pic.frame_height_minus1) + 1;

    if (av1->header.show_existing_frame) {
        if (!av1->pending_slices.empty()) {
            VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "AV1 show-existing frame must not carry pending tile parameters");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        av1->decode_tiles.clear();
        av1->decode_window_offset = 0;
        av1->decode_window_size   = 0;
        VKVV_SUCCESS_REASON(reason, reason_size, "captured AV1 show-existing frame: %ux%u profile=%u depth=%u fourcc=0x%x map_idx=%d display_frame_id=%u bitstream=%zu header=%u",
                            *width, *height, av1->pic.profile, bit_depth, av1_fourcc(bit_depth), av1->header.frame_to_show_map_idx, av1->header.display_frame_id,
                            av1->bitstream.size(), av1->header.frame_header_offset);
        return VA_STATUS_SUCCESS;
    }
    if (!av1->pending_slices.empty()) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "missing AV1 slice data for pending tile parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!sort_and_validate_tiles(av1, reason, reason_size)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    uint32_t window_offset = 0;
    if (!av1_decode_window(av1, &window_offset, reason, reason_size)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    uint32_t first_tile_offset = 0;
    if (!av1->decode_tiles.empty()) {
        first_tile_offset = av1->decode_tiles[0].offset;
    }
    VKVV_SUCCESS_REASON(reason, reason_size,
                        "captured AV1 picture: %ux%u profile=%u depth=%u fourcc=0x%x tiles=%zu bitstream=%zu decode=%zu window=%u frame=%u show=%u hdr_existing=%u hdr_show=%u "
                        "hdr_showable=%u refresh=0x%02x header=%u tile0=%u q=%u",
                        *width, *height, av1->pic.profile, bit_depth, av1_fourcc(bit_depth), av1->decode_tiles.size(), av1->bitstream.size(), av1->decode_window_size,
                        window_offset, av1->pic.pic_info_fields.bits.frame_type, av1->pic.pic_info_fields.bits.show_frame, av1->header.show_existing_frame ? 1U : 0U,
                        av1->header.show_frame ? 1U : 0U, av1->header.showable_frame ? 1U : 0U, av1->header.refresh_frame_flags, av1->header.frame_header_offset, first_tile_offset,
                        av1->pic.base_qindex);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_av1_get_decode_input(void* state, VkvvAV1DecodeInput* input) {
    if (input == nullptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    auto* av1 = static_cast<AV1State*>(state);
    if (av1 == nullptr || !av1->has_pic || !av1->has_slice_data || !av1->has_header) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    const bool presentation_only = av1->header.show_existing_frame;
    if (!presentation_only &&
        (av1->decode_tiles.empty() || av1->decode_window_size == 0 || av1->decode_window_offset > av1->bitstream.size() ||
         av1->decode_window_size > av1->bitstream.size() - av1->decode_window_offset)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    *input                      = {};
    input->pic                  = &av1->pic;
    input->tiles                = presentation_only ? nullptr : av1->decode_tiles.data();
    input->tile_count           = presentation_only ? 0 : av1->decode_tiles.size();
    input->bitstream            = presentation_only ? nullptr : av1->bitstream.data() + av1->decode_window_offset;
    input->bitstream_size       = presentation_only ? 0 : av1->decode_window_size;
    input->decode_window_offset = av1->decode_window_offset;
    input->header               = av1->header;
    input->sequence             = av1->sequence;
    input->bit_depth            = av1_bit_depth(av1->pic);
    input->rt_format            = av1_rt_format(input->bit_depth);
    input->fourcc               = av1_fourcc(input->bit_depth);
    input->frame_width          = static_cast<uint32_t>(av1->pic.frame_width_minus1) + 1;
    input->frame_height         = static_cast<uint32_t>(av1->pic.frame_height_minus1) + 1;
    return VA_STATUS_SUCCESS;
}
