// nv12_equalize_to_udp_and_mp4.cpp
// Fixes: proper MP4 finalize (parse -> mp4mux), wait-for-EOS on output pipeline,
// safer pipeline construction, explicit appsrc caps/timestamps, decodebin input.
// Builds on Linux with GStreamer 1.x and OpenCV 4.x
//
// Build:
// g++ -O3 -DNDEBUG -std=c++17 nv12_equalize_to_udp_and_mp4.cpp -o nv12_nv12_mp4 \
//   $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 opencv4) -lpthread
//
// Example:
// ./nv12_nv12_mp4 --input=sample.mp4 --output=out.mp4 --codec=h264 --bitrate=10000 \
//   --resolution=1280x720 --fps=30 --loop

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <glib.h>
#include <opencv2/opencv.hpp>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    GstElement *appsrc;
    GstElement *appsink;
    gboolean video_info_valid;
    GstVideoInfo video_info;
    GTimer *processing_timer;
    double total_processing_time;
    int frame_count;
    gchar *input_file;
    gchar *output_file;
    gboolean loop_playback;

    GMainLoop *loop;
    GstElement *sink_pipeline; // input pipeline (file -> decode -> appsink)
    GstElement *src_pipeline;  // output pipeline (appsrc -> enc -> tee -> udp/mp4)
    gboolean save_to_file;

    // === CLAHE controls ===
    double clip_limit;           // e.g., 2.0
    int    tile_grid;            // e.g., 8 means (8x8)
    cv::Ptr<cv::CLAHE> clahe;    // created once and reused

    GstClockTime frame_duration;
    GstClockTime current_timestamp;

    gboolean got_in_eos;   // EOS from input pipeline
    gboolean got_out_eos;  // EOS from output pipeline

} CustomData;

static void set_appsrc_caps(CustomData *data) {
    if (!data->video_info_valid || !data->appsrc) return;
    const int w = data->video_info.width;
    const int h = data->video_info.height;
    const int fn = data->video_info.fps_n > 0 ? data->video_info.fps_n : 30;
    const int fd = data->video_info.fps_d > 0 ? data->video_info.fps_d : 1;

    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        "width", G_TYPE_INT, w,
        "height", G_TYPE_INT, h,
        "framerate", GST_TYPE_FRACTION, fn, fd,
        NULL);
    gst_app_src_set_caps(GST_APP_SRC(data->appsrc), caps);
    gst_caps_unref(caps);
}

// Called when appsink has a new sample
static GstFlowReturn new_sample_cb(GstAppSink *appsink, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;

    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) {
        g_printerr("Failed to pull sample from appsink\n");
        return GST_FLOW_ERROR;
    }

    if (!data->video_info_valid) {
        GstCaps *caps = gst_sample_get_caps(sample);
        if (caps && gst_video_info_from_caps(&data->video_info, caps)) {
            data->video_info_valid = TRUE;
            g_print("Video info: %dx%d, format: %s\n",
                    data->video_info.width, data->video_info.height,
                    gst_video_format_to_string(data->video_info.finfo->format));

            if (data->video_info.fps_n > 0 && data->video_info.fps_d > 0) {
                data->frame_duration = gst_util_uint64_scale_int(GST_SECOND,
                                                                 data->video_info.fps_d,
                                                                 data->video_info.fps_n);
            } else {
                data->frame_duration = gst_util_uint64_scale_int(GST_SECOND, 1, 30);
            }
            data->current_timestamp = 0;
            set_appsrc_caps(data); // ensure appsrc caps match first sample
        } else {
            g_printerr("Failed to extract video info\n");
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }
    }

    if (data->frame_count == 0) {
        g_print("First frame received! Processing started.\n");
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        g_printerr("Failed to get buffer from sample\n");
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstMapInfo map_info;
    if (!gst_buffer_map(buffer, &map_info, GST_MAP_READ)) {
        g_printerr("Failed to map buffer\n");
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    const int width = data->video_info.width;
    const int height = data->video_info.height;

    const size_t y_size = (size_t)width * (size_t)height;
    const size_t uv_size = y_size / 2;
    if (map_info.size < y_size + uv_size) {
        g_printerr("Buffer size mismatch: expected %zu, got %zu\n", y_size + uv_size, map_info.size);
        gst_buffer_unmap(buffer, &map_info);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    try {
        g_timer_start(data->processing_timer);

        // NV12 -> Y plane -> equalize -> NV12(rebuild with UV=128)
        cv::Mat nv12_in(height * 3 / 2, width, CV_8UC1, map_info.data);
        cv::Mat y_in = nv12_in(cv::Rect(0, 0, width, height));
        cv::Mat y_out(height, width, CV_8UC1);
        if (!data->clahe) {
        data->clahe = cv::createCLAHE(data->clip_limit, cv::Size(data->tile_grid, data->tile_grid));
        // clamp guard (tile must be >= 1)
        if (data->tile_grid < 1) data->tile_grid = 1;
        data->clahe->setClipLimit(data->clip_limit);
        data->clahe->setTilesGridSize(cv::Size(data->tile_grid, data->tile_grid));
    }
    data->clahe->apply(y_in, y_out);

        cv::Mat nv12_out(height * 3 / 2, width, CV_8UC1);
        memcpy(nv12_out.data, y_out.data, y_size);
        memset(nv12_out.data + y_size, 128, uv_size);

        g_timer_stop(data->processing_timer);
        const double ms = g_timer_elapsed(data->processing_timer, NULL) * 1000.0;
        data->total_processing_time += ms;
        data->frame_count++;
        if ((data->frame_count % 100) == 0) {
            const double avg = data->total_processing_time / data->frame_count;
            g_print("Stats - Frame %d: %.2f ms, avg: %.2f ms, FPS: %.1f\n",
                    data->frame_count, ms, avg, avg > 0.0 ? 1000.0 / avg : 0.0);
        }

        // Create output buffer
        GstBuffer *out = gst_buffer_new_allocate(NULL, y_size + uv_size, NULL);
        if (!out) {
            g_printerr("Failed to allocate processed buffer\n");
            gst_buffer_unmap(buffer, &map_info);
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }

        GstMapInfo wmap;
        if (gst_buffer_map(out, &wmap, GST_MAP_WRITE)) {
            memcpy(wmap.data, nv12_out.data, y_size + uv_size);
            gst_buffer_unmap(out, &wmap);

            GST_BUFFER_PTS(out) = data->current_timestamp;
            GST_BUFFER_DTS(out) = data->current_timestamp;
            GST_BUFFER_DURATION(out) = data->frame_duration;
            data->current_timestamp += data->frame_duration;

            GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), out);
            if (ret != GST_FLOW_OK) {
                g_printerr("Failed to push buffer to appsrc: %d\n", ret);
                gst_buffer_unref(out);
            }
        } else {
            g_printerr("Failed to map processed buffer\n");
            gst_buffer_unref(out);
        }

    } catch (const std::exception &e) {
        g_printerr("OpenCV error: %s\n", e.what());
        gst_buffer_unmap(buffer, &map_info);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    gst_buffer_unmap(buffer, &map_info);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static gboolean bus_message_cb(GstBus *bus, GstMessage *message, gpointer user_data) {
    (void)bus;
    CustomData *data = (CustomData *)user_data;

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_EOS: {
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->sink_pipeline)) {
                g_print("[sink] End of input stream.\n");
                data->got_in_eos = TRUE;
                if (data->loop_playback) {
                    g_print("Restarting playback...\n");
                    data->current_timestamp = 0; // reset for next loop
                    if (!gst_element_seek_simple(
                            data->sink_pipeline, GST_FORMAT_TIME,
                            (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0)) {
                        g_printerr("Seek failed; stopping.\n");
                        if (data->loop) g_main_loop_quit(data->loop);
                    }
                } else {
                    if (data->appsrc) {
                        gst_app_src_end_of_stream(GST_APP_SRC(data->appsrc));
                    }
                    g_print("Signaled EOS to appsrc (output pipeline). Waiting for mp4 finalize...\n");
                }
            } else if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->src_pipeline)) {
                g_print("[src] Output pipeline EOS.\n");
                data->got_out_eos = TRUE;
                if (data->save_to_file && data->output_file) {
                    g_print("MP4 output file saved: %s\n", data->output_file);
                }
                if (data->loop) g_main_loop_quit(data->loop);
            } else {
                // Generic EOS from something else; only quit if not looping and we already saw src EOS
                if (!data->loop_playback) {
                    if (data->loop) g_main_loop_quit(data->loop);
                }
            }
            break;
        }

        case GST_MESSAGE_ERROR: {
            GError *err = NULL; gchar *dbg = NULL;
            gst_message_parse_error(message, &err, &dbg);
            g_printerr("Error from %s: %s\n", GST_OBJECT_NAME(message->src), err ? err->message : "unknown");
            g_printerr("Debug info: %s\n", dbg ? dbg : "none");
            if (err) g_error_free(err); if (dbg) g_free(dbg);
            if (data->loop) g_main_loop_quit(data->loop);
            break;
        }

        case GST_MESSAGE_WARNING: {
            GError *err = NULL; gchar *dbg = NULL;
            gst_message_parse_warning(message, &err, &dbg);
            g_print("Warning from %s: %s\n", GST_OBJECT_NAME(message->src), err ? err->message : "unknown");
            g_print("Debug info: %s\n", dbg ? dbg : "none");
            if (err) g_error_free(err); if (dbg) g_free(dbg);
            break;
        }

        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->sink_pipeline) ||
                GST_MESSAGE_SRC(message) == GST_OBJECT(data->src_pipeline)) {
                GstState old_s, new_s, pend;
                gst_message_parse_state_changed(message, &old_s, &new_s, &pend);
                g_print("Pipeline state changed: %s -> %s\n",
                        gst_element_state_get_name(old_s),
                        gst_element_state_get_name(new_s));
            }
            break;
        }

        default: break;
    }
    return TRUE;
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    gboolean use_h265 = FALSE;
    int bitrate_kbps = 25000;
    gchar *input_file = NULL;
    gchar *output_file = NULL;
    gboolean loop_playback = FALSE;
    int target_width = 1280;
    int target_height = 720;
    int target_fps_num = 30;
    int target_fps_den = 1;
    gboolean udp_only = FALSE;
    double clip_limit = 2.0;  // default CLAHE clip limit
    int    tile_grid  = 8;    // default CLAHE tile grid size (tile x tile)

    for (int i = 1; i < argc; ++i) {
        if (g_str_has_prefix(argv[i], "--codec=")) {
            const char *val = strchr(argv[i], '=');
            if (val && g_ascii_strcasecmp(val + 1, "h265") == 0) use_h265 = TRUE;
        } else if (g_strcmp0(argv[i], "--codec") == 0 && i + 1 < argc) {
            if (g_ascii_strcasecmp(argv[i + 1], "h265") == 0) use_h265 = TRUE; i++;
        } else if (g_str_has_prefix(argv[i], "--bitrate=")) {
            const char *val = strchr(argv[i], '='); if (val) { int v = atoi(val + 1); if (v > 0) bitrate_kbps = v; }
        } else if (g_strcmp0(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            int v = atoi(argv[i + 1]); if (v > 0) bitrate_kbps = v; i++;
        } else if (g_str_has_prefix(argv[i], "--input=")) {
            const char *val = strchr(argv[i], '='); if (val) input_file = g_strdup(val + 1);
        } else if (g_strcmp0(argv[i], "--input") == 0 && i + 1 < argc) {
            input_file = g_strdup(argv[i + 1]); i++;
        } else if (g_str_has_prefix(argv[i], "--output=")) {
            const char *val = strchr(argv[i], '='); if (val) output_file = g_strdup(val + 1);
        } else if (g_strcmp0(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = g_strdup(argv[i + 1]); i++;
        } else if (g_strcmp0(argv[i], "--loop") == 0) {
            loop_playback = TRUE;
        } else if (g_strcmp0(argv[i], "--udp-only") == 0) {
            udp_only = TRUE;
        } else if (g_str_has_prefix(argv[i], "--resolution=")) {
            const char *val = strchr(argv[i], '=');
            if (val) {
                if (sscanf(val + 1, "%dx%d", &target_width, &target_height) != 2) {
                    g_printerr("Invalid resolution format. Use --resolution=WIDTHxHEIGHT\n");
                }
            }
        } else if (g_str_has_prefix(argv[i], "--fps=")) {
            const char *val = strchr(argv[i], '=');
            if (val) {
                if (sscanf(val + 1, "%d/%d", &target_fps_num, &target_fps_den) != 2) {
                    target_fps_num = atoi(val + 1); target_fps_den = 1;
                }
            }
        } else if (g_str_has_prefix(argv[i], "--clipLimit=")) {
            const char *val = strchr(argv[i], '=');
            if (val) {
                double v = g_ascii_strtod(val + 1, NULL);
                if (v > 0.0) clip_limit = v;
            }
        } else if (g_strcmp0(argv[i], "--clipLimit") == 0 && i + 1 < argc) {
            double v = g_ascii_strtod(argv[i + 1], NULL);
            if (v > 0.0) clip_limit = v; i++;
        } else if (g_str_has_prefix(argv[i], "--tile=")) {
            const char *val = strchr(argv[i], '=');
            if (val) {
                int v = atoi(val + 1);
                if (v >= 1) tile_grid = v;
            }
        } else if (g_strcmp0(argv[i], "--tile") == 0 && i + 1 < argc) {
            int v = atoi(argv[i + 1]);
            if (v >= 1) tile_grid = v; i++;
        }
    }
        
    

    if (!input_file) {
        g_printerr("Usage: %s --input=/path/to/video.mp4 [OPTIONS]\n", argv[0]);
        g_printerr("Options:\n");
        g_printerr("  --codec=h264|h265     Encoder codec (default: h264)\n");
        g_printerr("  --bitrate=N           Bitrate in kbps (default: 25000)\n");
        g_printerr("  --resolution=WxH      Target resolution (default: 1280x720)\n");
        g_printerr("  --fps=N or N/D        Target framerate (default: 30/1)\n");
        g_printerr("  --output=file.mp4     Output MP4 file path (optional)\n");
        g_printerr("  --udp-only            Only stream via UDP, no file output\n");
        g_printerr("  --loop                Loop playback");
        g_printerr("  --clipLimit=F        CLAHE clip limit (default: 2.0)");
        g_printerr("  --tile=N             CLAHE tiles grid size NxN (default: 8)");
        return -1;
    }

    if (!g_file_test(input_file, G_FILE_TEST_EXISTS)) {
        g_printerr("Error: Input file '%s' does not exist\n", input_file);
        g_free(input_file);
        return -1;
    }

    if (!udp_only && !output_file) {
        gchar *base = g_path_get_basename(input_file);
        gchar *name_noext = NULL; gchar *dot = g_strrstr(base, ".");
        name_noext = dot ? g_strndup(base, dot - base) : g_strdup(base);
        output_file = g_strdup_printf("%s_processed.mp4", name_noext);
        g_free(base); g_free(name_noext);
    }

    g_print("Input: %s\n", input_file);
    if (!udp_only) g_print("Output: %s\n", output_file); else g_print("Output: UDP stream only\n");
    g_print("Target resolution: %dx%d @ %d/%d fps\n", target_width, target_height, target_fps_num, target_fps_den);
    g_print("Encoder: %s, target-bitrate: %d kbps\n", use_h265 ? "H.265" : "H.264", bitrate_kbps);
    g_print("Loop playback: %s\n", loop_playback ? "enabled" : "disabled");

    CustomData data = {};
    data.video_info_valid = FALSE;
    data.processing_timer = g_timer_new();
    data.total_processing_time = 0.0;
    data.frame_count = 0;
    data.input_file = input_file;
    data.output_file = output_file;
    data.loop_playback = loop_playback;
    data.loop = NULL;
    data.sink_pipeline = NULL;
    data.src_pipeline = NULL;
    data.save_to_file = !udp_only;
    data.current_timestamp = 0;

    // Initialize CLAHE
    data.clip_limit = clip_limit;
    data.tile_grid  = tile_grid < 1 ? 1 : tile_grid;
    data.clahe      = cv::createCLAHE(data.clip_limit, cv::Size(data.tile_grid, data.tile_grid));

    g_print("CLAHE: clipLimit=%.3f, tileGrid=%dx%d", data.clip_limit, data.tile_grid, data.tile_grid);
    data.frame_duration = 0;
    data.got_in_eos = FALSE; data.got_out_eos = FALSE;

    GError *error = NULL;

    // 1) Input pipeline: filesrc -> decodebin -> NV12 WxH fps -> appsink
    gchar *sink_pipeline_str = g_strdup_printf(
        "filesrc location=%s ! decodebin ! videoconvert ! videoscale ! videorate ! "
        "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/%d ! "
        "appsink name=my_sink emit-signals=true max-buffers=5 drop=true sync=false",
        input_file, target_width, target_height, target_fps_num, target_fps_den);

    GstElement *sink_pipeline = gst_parse_launch(sink_pipeline_str, &error);
    g_free(sink_pipeline_str);
    if (!sink_pipeline) {
        g_printerr("Failed to create sink pipeline: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        g_free(input_file); if (output_file) g_free(output_file);
        return -1;
    }

    data.appsink = gst_bin_get_by_name(GST_BIN(sink_pipeline), "my_sink");
    data.sink_pipeline = sink_pipeline;

    // 2) Output pipeline
    gchar *src_pipeline_str = NULL;
    if (udp_only) {
        if (use_h265) {
            src_pipeline_str = g_strdup_printf(
                "appsrc name=my_src is-live=true block=true format=GST_FORMAT_TIME do-timestamp=false ! "
                "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/%d ! queue ! "
                "omxh265enc num-slices=8 periodicity-idr=240 cpb-size=500 gdr-mode=horizontal initial-delay=250 "
                "control-rate=low-latency prefetch-buffer=true target-bitrate=%d gop-mode=low-delay-p ! "
                "h265parse config-interval=-1 ! rtph265pay pt=96 ! "
                "udpsink buffer-size=60000000 host=192.168.25.69 port=5004 sync=true async=false qos-dscp=60",
                target_width, target_height, target_fps_num, target_fps_den, bitrate_kbps);
        } else {
            src_pipeline_str = g_strdup_printf(
                "appsrc name=my_src is-live=true block=true format=GST_FORMAT_TIME do-timestamp=false ! "
                "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/%d ! queue ! "
                "omxh264enc num-slices=8 periodicity-idr=240 cpb-size=500 gdr-mode=horizontal initial-delay=250 "
                "control-rate=low-latency prefetch-buffer=true target-bitrate=%d gop-mode=low-delay-p ! "
                "h264parse config-interval=-1 ! rtph264pay pt=96 ! "
                "udpsink buffer-size=60000000 host=192.168.25.69 port=5004 sync=true async=false qos-dscp=60",
                target_width, target_height, target_fps_num, target_fps_den, bitrate_kbps);
        }
    } else {
        if (use_h265) {
            src_pipeline_str = g_strdup_printf(
                "appsrc name=my_src is-live=false block=true format=GST_FORMAT_TIME do-timestamp=false ! "
                "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/%d ! queue ! "
                "omxh265enc num-slices=8 periodicity-idr=240 cpb-size=500 gdr-mode=horizontal initial-delay=250 "
                "control-rate=constant target-bitrate=%d gop-mode=low-delay-p ! tee name=t ! "
                "queue ! h265parse config-interval=-1 ! rtph265pay pt=96 ! "
                "udpsink buffer-size=60000000 host=192.168.25.69 port=5004 sync=false async=true qos-dscp=60 "
                "t. ! queue ! h265parse ! video/x-h265,stream-format=hvc1,alignment=au ! mp4mux faststart=true ! "
                "filesink location=%s sync=false",
                target_width, target_height, target_fps_num, target_fps_den, bitrate_kbps, output_file);
        } else {
            src_pipeline_str = g_strdup_printf(
                "appsrc name=my_src is-live=false block=true format=GST_FORMAT_TIME do-timestamp=false ! "
                "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/%d ! queue ! "
                "omxh264enc num-slices=8 periodicity-idr=240 cpb-size=500 gdr-mode=horizontal initial-delay=250 "
                "control-rate=constant target-bitrate=%d gop-mode=low-delay-p ! tee name=t ! "
                "queue ! h264parse config-interval=-1 ! rtph264pay pt=96 ! "
                "udpsink buffer-size=60000000 host=192.168.25.69 port=5004 sync=false async=true qos-dscp=60 "
                "t. ! queue ! h264parse ! video/x-h264,stream-format=avc,alignment=au ! mp4mux faststart=true ! "
                "filesink location=%s sync=false",
                target_width, target_height, target_fps_num, target_fps_den, bitrate_kbps, output_file);
        }
    }

    GstElement *src_pipeline = gst_parse_launch(src_pipeline_str, &error);
    g_free(src_pipeline_str);
    if (!src_pipeline) {
        g_printerr("Failed to create src pipeline: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        gst_object_unref(sink_pipeline);
        g_free(input_file); if (output_file) g_free(output_file);
        return -1;
    }

    data.appsrc = gst_bin_get_by_name(GST_BIN(src_pipeline), "my_src");
    data.src_pipeline = src_pipeline;

    // Explicit caps on appsrc (also keep downstream capsfilter for safety)
    gst_util_set_object_arg(G_OBJECT(data.appsrc), "format", "time");
    if (data.video_info_valid) set_appsrc_caps(&data);

    // Register callbacks
    g_signal_connect(data.appsink, "new-sample", G_CALLBACK(new_sample_cb), &data);

    // Buses & main loop
    GstBus *sink_bus = gst_element_get_bus(sink_pipeline);
    GstBus *src_bus = gst_element_get_bus(src_pipeline);
    GMainLoop *main_loop = g_main_loop_new(NULL, FALSE);
    data.loop = main_loop;
    gst_bus_add_watch(sink_bus, bus_message_cb, &data);
    gst_bus_add_watch(src_bus, bus_message_cb, &data);

    g_print("Processing video file. Press Ctrl+C to exit.\n");

    // Start pipelines
    if (gst_element_set_state(src_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        g_printerr("Failed to start src pipeline\n");
    if (gst_element_set_state(sink_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        g_printerr("Failed to start sink pipeline\n");

    g_main_loop_run(main_loop);

    // Cleanup
    gst_object_unref(sink_bus);
    gst_object_unref(src_bus);
    gst_element_set_state(sink_pipeline, GST_STATE_NULL);
    gst_element_set_state(src_pipeline, GST_STATE_NULL);
    if (data.appsink) gst_object_unref(data.appsink);
    if (data.appsrc) gst_object_unref(data.appsrc);
    gst_object_unref(sink_pipeline);
    gst_object_unref(src_pipeline);
    g_timer_destroy(data.processing_timer);
    g_free(input_file);
    if (output_file) g_free(output_file);
    g_main_loop_unref(main_loop);

    return 0;
}
