// relay_debug_nv12_worker_opencl_fpga.cpp
// Worker-based processing with OpenCL FPGA histogram equalization + queue-level probes.
//
// Build:
// g++ -O3 -DNDEBUG -std=c++17 relay_debug_nv12_worker_opencl_fpga.cpp -o relay_debug_worker_opencl_fpga \
//   $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 opencv4) \
//   -lOpenCL -lpthread -lxilinxopencl

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <glib.h>
#include <opencv2/opencv.hpp>
#include <atomic>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <chrono>

// OpenCL includes
#define CL_HPP_CL_1_2_DEFAULT_BUILD
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_HPP_ENABLE_PROGRAM_CONSTRUCTION_FROM_ARRAY_COMPATIBILITY 1
#include <CL/cl2.hpp>

// Xilinx OpenCL utilities (assuming xcl2.hpp is available)
#include "xcl2.hpp"

// FPGA kernel configuration constants
#define WIDTH_4k 3840
#define HEIGHT_4k 2160
#define WIDTH_2k 1920
#define HEIGHT_2k 1080
#define INPUT_PTR_WIDTH 256
#define OUTPUT_PTR_WIDTH 256

struct Counters {
    // Camera queue (q_cam)
    std::atomic<uint64_t> cam_out_frames{0},     cam_out_bytes{0};
    std::atomic<uint64_t> qcam_out_frames{0},    qcam_out_bytes{0};

    // Appsink pad
    std::atomic<uint64_t> appsink_in_frames{0},  appsink_in_bytes{0};

    // Callback → enqueue
    std::atomic<uint64_t> enqueued_frames{0},    enqueued_bytes{0};

    // Worker processed / pushed
    std::atomic<uint64_t> processed_frames{0},   processed_bytes{0};
    std::atomic<uint64_t> after_src_frames{0},   after_src_bytes{0};
    std::atomic<uint64_t> encoder_in_frames{0},  encoder_in_bytes{0};

    std::atomic<uint64_t> push_failures{0};
    std::atomic<uint64_t> processing_errors{0};

    // OpenCL FPGA processing timing
    std::atomic<uint64_t> total_processing_time_us{0};
    std::atomic<uint64_t> opencl_errors{0};
};

struct SharedOpenCLContext {
    cl::Context context;
    cl::Device device;
    cl::Program program;
    bool initialized{false};
    GMutex mutex; // Protect shared resources
};

struct WorkerOpenCLContext {
    cl::CommandQueue queue;
    cl::Kernel kernel;
    
    // Pre-allocated buffers for efficiency
    cl::Buffer img_y_in_buffer;
    cl::Buffer img_y_ref_buffer;
    cl::Buffer img_y_out_buffer;
    size_t buffer_size{0};
    bool initialized{false};
};

struct CustomData {
    GstElement  *appsrc{nullptr};
    GstElement  *appsink{nullptr};
    gboolean     video_info_valid{FALSE};
    GstVideoInfo video_info{};

    // Worker decoupling
    GAsyncQueue *work_q{nullptr};   // carries GstBuffer* from callback to worker
    GThread     *worker{nullptr};
    std::atomic<bool> stop{false};

    // Multi-threading support
    int num_workers{1};
    GThread     **workers{nullptr};
    SharedOpenCLContext shared_opencl{}; // Shared context and program
    WorkerOpenCLContext *worker_opencl_contexts{nullptr}; // Per-worker queues and buffers

    Counters     ctr{};
    GMainLoop   *loop{nullptr};
};

/* ---------- OpenCL FPGA Initialization ---------- */

static bool initialize_shared_opencl_context(SharedOpenCLContext* shared_ctx) {
    try {
        // Get Xilinx FPGA devices
        std::vector<cl::Device> devices = xcl::get_xil_devices();
        if (devices.empty()) {
            g_printerr("No Xilinx FPGA devices found\n");
            return false;
        }
        
        shared_ctx->device = devices[0];
        shared_ctx->context = cl::Context(shared_ctx->device);
        
        // Load the FPGA binary once for all workers
        std::string device_name = shared_ctx->device.getInfo<CL_DEVICE_NAME>();
        std::string binaryFile = xcl::find_binary_file(device_name, "krnl_hist_equalize");
        cl::Program::Binaries bins = xcl::import_binary_file(binaryFile);
        
        std::vector<cl::Device> prog_devices = {shared_ctx->device};
        shared_ctx->program = cl::Program(shared_ctx->context, prog_devices, bins);
        
        // Initialize mutex for thread safety
        g_mutex_init(&shared_ctx->mutex);
        
        shared_ctx->initialized = true;
        g_print("Shared OpenCL FPGA context initialized successfully\n");
        return true;
        
    } catch (const GError& e) {
      g_printerr("OpenCL initialization error");
      return false;
        }catch (const std::exception& e) {
        g_printerr("Exception during shared OpenCL initialization: %s\n", e.what());
        return false;
    }
}

static bool initialize_worker_opencl_context(WorkerOpenCLContext* ctx, SharedOpenCLContext* shared_ctx, int worker_id) {
    try {
        // Create worker-specific command queue
        ctx->queue = cl::CommandQueue(shared_ctx->context, shared_ctx->device, CL_QUEUE_PROFILING_ENABLE);
        
        // Create worker-specific kernel instance
        ctx->kernel = cl::Kernel(shared_ctx->program, "equalizeHist_accel");
        
        ctx->initialized = true;
        g_print("Worker %d: OpenCL context initialized successfully\n", worker_id);
        return true;
        
    } catch (const GError& e) {
      g_printerr("OpenCL initialization error");
    }
    catch (const std::exception& e) {
        g_printerr("Worker %d: Exception during OpenCL initialization: %s\n", worker_id, e.what());
        return false;
    }
}

static void cleanup_shared_opencl_context(SharedOpenCLContext* ctx) {
    if (ctx->initialized) {
        g_mutex_clear(&ctx->mutex);
        ctx->initialized = false;
    }
}

static void cleanup_worker_opencl_context(WorkerOpenCLContext* ctx) {
    // OpenCL objects are automatically cleaned up by destructors
    ctx->initialized = false;
}

static bool allocate_worker_opencl_buffers(WorkerOpenCLContext* ctx, SharedOpenCLContext* shared_ctx, size_t y_size) {
    if (ctx->buffer_size == y_size && ctx->img_y_in_buffer() != nullptr) {
        return true; // Buffers already allocated for this size
    }
    
    try {
        // Allocate new buffers using shared context
        ctx->img_y_in_buffer = cl::Buffer(shared_ctx->context, CL_MEM_READ_ONLY, y_size);
        ctx->img_y_ref_buffer = cl::Buffer(shared_ctx->context, CL_MEM_READ_ONLY, y_size);
        ctx->img_y_out_buffer = cl::Buffer(shared_ctx->context, CL_MEM_WRITE_ONLY, y_size);
        ctx->buffer_size = y_size;
        return true;
        
    } catch (const GError& e) {
      g_printerr("OpenCL initialization error");
      return false;
    }
}

/* ---------- Pad probes ---------- */

static GstPadProbeReturn probe_cam_out(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    auto *d = (CustomData*)user_data;
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) && GST_PAD_PROBE_INFO_BUFFER(info)) {
        GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info);
        d->ctr.cam_out_frames.fetch_add(1, std::memory_order_relaxed);
        d->ctr.cam_out_bytes .fetch_add(gst_buffer_get_size(b), std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn probe_qcam_out(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    auto *d = (CustomData*)user_data;
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) && GST_PAD_PROBE_INFO_BUFFER(info)) {
        GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info);
        d->ctr.qcam_out_frames.fetch_add(1, std::memory_order_relaxed);
        d->ctr.qcam_out_bytes .fetch_add(gst_buffer_get_size(b), std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn probe_apps_sink(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    auto *d = (CustomData*)user_data;
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) && GST_PAD_PROBE_INFO_BUFFER(info)) {
        GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info);
        d->ctr.appsink_in_frames.fetch_add(1, std::memory_order_relaxed);
        d->ctr.appsink_in_bytes .fetch_add(gst_buffer_get_size(b), std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn probe_after_appsrc_queue(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    auto *d = (CustomData*)user_data;
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) && GST_PAD_PROBE_INFO_BUFFER(info)) {
        GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info);
        d->ctr.after_src_frames.fetch_add(1, std::memory_order_relaxed);
        d->ctr.after_src_bytes .fetch_add(gst_buffer_get_size(b), std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn probe_encoder_sink(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    auto *d = (CustomData*)user_data;
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) && GST_PAD_PROBE_INFO_BUFFER(info)) {
        GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info);
        d->ctr.encoder_in_frames.fetch_add(1, std::memory_order_relaxed);
        d->ctr.encoder_in_bytes .fetch_add(gst_buffer_get_size(b), std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

/* ---------- appsink callback: O(1) enqueue ---------- */

static GstFlowReturn new_sample_cb(GstAppSink *appsink, gpointer user_data) {
    auto *d = (CustomData *)user_data;

    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_ERROR;
    GstBuffer *inbuf = gst_sample_get_buffer(sample);
    if (!inbuf) { gst_sample_unref(sample); return GST_FLOW_ERROR; }

    // Cache caps once (diagnostic only)
    if (!d->video_info_valid) {
        if (GstCaps *caps = gst_sample_get_caps(sample)) {
            if (gst_video_info_from_caps(&d->video_info, caps)) {
                d->video_info_valid = TRUE;
                g_print("Video info: %dx%d\n", d->video_info.width, d->video_info.height);
            }
        }
    }

    // O(1): ref buffer, queue to worker, unref sample (returns one ref to pool)
    gst_buffer_ref(inbuf);
    g_async_queue_push(d->work_q, inbuf);
    d->ctr.enqueued_frames.fetch_add(1, std::memory_order_relaxed);
    d->ctr.enqueued_bytes .fetch_add(gst_buffer_get_size(inbuf), std::memory_order_relaxed);

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* ---------- worker thread: OpenCL FPGA histogram equalization + push ---------- */

static gpointer worker_thread_fn(gpointer user_data) {
    struct WorkerData {
        CustomData* main_data;
        int worker_id;
    };
    
    WorkerData* worker_data = (WorkerData*)user_data;
    CustomData* d = worker_data->main_data;
    int worker_id = worker_data->worker_id;
    
    // Initialize worker OpenCL context for this worker
    WorkerOpenCLContext* ctx = &d->worker_opencl_contexts[worker_id];
    if (!initialize_worker_opencl_context(ctx, &d->shared_opencl, worker_id)) {
        g_printerr("Worker %d: Failed to initialize OpenCL context, exiting\n", worker_id);
        return nullptr;
    }

    g_print("Worker %d: Started successfully\n", worker_id);

    while (!d->stop.load(std::memory_order_acquire)) {
        // Pop with timeout to allow graceful exit
        gpointer item = g_async_queue_timeout_pop(d->work_q, 50 * G_TIME_SPAN_MILLISECOND);
        if (!item) continue;

        GstBuffer *inbuf = (GstBuffer*)item;

        try {
            // Map input buffer for reading
            GstMapInfo map_info;
            if (!gst_buffer_map(inbuf, &map_info, GST_MAP_READ)) {
                gst_buffer_unref(inbuf);
                d->ctr.processing_errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (!d->video_info_valid) {
                gst_buffer_unmap(inbuf, &map_info);
                gst_buffer_unref(inbuf);
                continue;
            }

            int width = d->video_info.width;
            int height = d->video_info.height;
            size_t y_size = (size_t)width * (size_t)height;
            size_t uv_size = (size_t)width * (size_t)height / 2;

            if (map_info.size < y_size + uv_size) {
                gst_buffer_unmap(inbuf, &map_info);
                gst_buffer_unref(inbuf);
                d->ctr.processing_errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            auto start_time = std::chrono::high_resolution_clock::now();

            // Extract Y plane from NV12
            cv::Mat nv12_input(height * 3 / 2, width, CV_8UC1, map_info.data);
            cv::Mat y_plane_in = nv12_input(cv::Rect(0, 0, width, height)).clone();
            cv::Mat y_plane_out(height, width, CV_8UC1);

            // Allocate OpenCL buffers if needed
            if (!allocate_worker_opencl_buffers(ctx, &d->shared_opencl, y_size)) {
                gst_buffer_unmap(inbuf, &map_info);
                gst_buffer_unref(inbuf);
                d->ctr.opencl_errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // OpenCL FPGA Histogram Equalization
            try {
                // Set kernel arguments
                ctx->kernel.setArg(0, ctx->img_y_in_buffer);
                ctx->kernel.setArg(1, ctx->img_y_ref_buffer);  // Using same input as reference
                ctx->kernel.setArg(2, ctx->img_y_out_buffer);
                ctx->kernel.setArg(3, height);
                ctx->kernel.setArg(4, width);

                // Transfer data to FPGA
                ctx->queue.enqueueWriteBuffer(ctx->img_y_in_buffer, CL_TRUE, 0, y_size, y_plane_in.data);
                ctx->queue.enqueueWriteBuffer(ctx->img_y_ref_buffer, CL_TRUE, 0, y_size, y_plane_in.data);
                
                // Execute kernel on FPGA
                ctx->queue.enqueueTask(ctx->kernel);
                ctx->queue.finish();
                
                // Read result back from FPGA
                ctx->queue.enqueueReadBuffer(ctx->img_y_out_buffer, CL_TRUE, 0, y_size, y_plane_out.data);
                ctx->queue.finish();

            } catch (const GError& e) {
                g_printerr("OpenCL initialization error");
                continue;
            }
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            d->ctr.total_processing_time_us.fetch_add(duration.count(), std::memory_order_relaxed);

            // Create output buffer
            GstBuffer *outbuf = gst_buffer_new_allocate(NULL, y_size + uv_size, NULL);
            if (!outbuf) {
                gst_buffer_unmap(inbuf, &map_info);
                gst_buffer_unref(inbuf);
                d->ctr.processing_errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // Map output buffer and reconstruct NV12
            GstMapInfo out_map_info;
            if (gst_buffer_map(outbuf, &out_map_info, GST_MAP_WRITE)) {
                // Copy processed Y plane
                memcpy(out_map_info.data, y_plane_out.data, y_size);
                // Fill UV with neutral value 128 (like original code)
                memset(out_map_info.data + y_size, 128, uv_size);
                gst_buffer_unmap(outbuf, &out_map_info);
            } else {
                gst_buffer_unref(outbuf);
                gst_buffer_unmap(inbuf, &map_info);
                gst_buffer_unref(inbuf);
                d->ctr.processing_errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            gst_buffer_unmap(inbuf, &map_info);
            gst_buffer_unref(inbuf); // release camera buffer

            // Fresh timestamps in appsrc pipeline
            GST_BUFFER_PTS(outbuf)      = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DTS(outbuf)      = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DURATION(outbuf) = GST_CLOCK_TIME_NONE;

            d->ctr.processed_frames.fetch_add(1, std::memory_order_relaxed);
            d->ctr.processed_bytes .fetch_add(gst_buffer_get_size(outbuf), std::memory_order_relaxed);

            GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(d->appsrc), outbuf);
            if (ret != GST_FLOW_OK) {
                d->ctr.push_failures.fetch_add(1, std::memory_order_relaxed);
                gst_buffer_unref(outbuf);
            }

        } catch (const std::exception& e) {
            gst_buffer_unref(inbuf);
            d->ctr.processing_errors.fetch_add(1, std::memory_order_relaxed);
            g_printerr("Worker %d: Processing error: %s\n", worker_id, e.what());
        }
    }
    
    // Cleanup worker OpenCL context
    cleanup_worker_opencl_context(ctx);
    g_print("Worker %d: Exiting\n", worker_id);
    return nullptr;
}

/* ---------- periodic status ---------- */

// Static variables to track previous values for rate calculation
static uint64_t prev_cam_out = 0;
static uint64_t prev_apps_in = 0;
static uint64_t prev_processed = 0;
static uint64_t prev_encoder_in = 0;
static uint64_t prev_encoder_bytes = 0;

static gboolean status_tick(gpointer user_data) {
    auto *d = (CustomData*)user_data;

    const uint64_t cam_out = d->ctr.cam_out_frames.load();
    const uint64_t apps_in = d->ctr.appsink_in_frames.load();
    const uint64_t processed = d->ctr.processed_frames.load();
    const uint64_t encoder_in = d->ctr.encoder_in_frames.load();
    const uint64_t encoder_bytes = d->ctr.encoder_in_bytes.load();

    const int qlen = g_async_queue_length(d->work_q);
    const uint64_t proc_errors = d->ctr.processing_errors.load();
    const uint64_t opencl_errors = d->ctr.opencl_errors.load();
    const uint64_t total_proc_time = d->ctr.total_processing_time_us.load();

    // Calculate frame rates (frames per 2 seconds, so divide by 2 for fps)
    double camera_fps = (cam_out - prev_cam_out) / 2.0;
    double opencv_input_fps = (apps_in - prev_apps_in) / 2.0;
    double opencv_output_fps = (processed - prev_processed) / 2.0;
    double encoder_input_fps = (encoder_in - prev_encoder_in) / 2.0;
    
    // Calculate output bitrate (bytes per 2 seconds, convert to kbps)
    double output_bitrate_kbps = (encoder_bytes - prev_encoder_bytes) * 8.0 / (2.0 * 1000.0);

    double avg_proc_time_ms = 0.0;
    if (processed > 0) {
        avg_proc_time_ms = (double)total_proc_time / (double)processed / 1000.0; // convert µs to ms
    }

    // Determine processing status
    const char* processing_status;
    if (opencl_errors > 0) {
        processing_status = "FPGA ERRORS";
    } else if (proc_errors > 0) {
        processing_status = "PROCESSING ERRORS";
    } else if (qlen > 5) {
        processing_status = "QUEUE BACKLOG";
    } else if (opencv_output_fps > 0) {
        processing_status = "ACTIVE";
    } else {
        processing_status = "IDLE";
    }

    g_print(
        "\n=== FRAME RATE MONITORING (every 2s) ===\n"
        "Camera Capture Rate: %6.1f fps\n"
        "OpenCV Input Rate:   %6.1f fps\n"
        "OpenCV Output Rate:  %6.1f fps\n"
        "Encoder Input Rate:  %6.1f fps\n"
        "Output Bitrate:      %6.1f kbps\n"
        "\n"
        "Queue Length: %d | Processing Errors: %" G_GUINT64_FORMAT " | Avg Process Time: %.2f ms\n"
        "Processing Status: %s (workers=%d, avg_frame_time=%.1fms)\n",
        camera_fps,
        opencv_input_fps, 
        opencv_output_fps,
        encoder_input_fps,
        output_bitrate_kbps,
        qlen, proc_errors + opencl_errors, avg_proc_time_ms,
        processing_status, d->num_workers, avg_proc_time_ms
    );

    // Update previous values for next iteration
    prev_cam_out = cam_out;
    prev_apps_in = apps_in;
    prev_processed = processed;
    prev_encoder_in = encoder_in;
    prev_encoder_bytes = encoder_bytes;

    return TRUE;
}

/* ---------- bus watch (quit main loop) ---------- */

static gboolean bus_cb(GstBus *bus, GstMessage *msg, gpointer user_data) {
    auto *d = (CustomData*)user_data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *e=NULL; gchar *dbg=NULL;
            gst_message_parse_error(msg, &e, &dbg);
            g_printerr("ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), e->message);
            g_error_free(e); g_free(dbg);
            if (d->loop) g_main_loop_quit(d->loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("EOS from %s\n", GST_OBJECT_NAME(msg->src));
            if (d->loop) g_main_loop_quit(d->loop);
            break;
        default: break;
    }
    return TRUE;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    gst_init(&argc, &argv);

    gboolean use_h265 = FALSE;
    int bitrate_kbps = 20000; // Match original default (20 Mbps)
    int num_workers = 2; // Default to 2 workers for better FPGA utilization

    int v_width = 1920, v_height = 1080, fps = 60; // defaults

    // --- extend argv parsing with width/height/fps ---
    for (int i=1;i<argc;++i){
        if (g_str_has_prefix(argv[i],"--codec=")) { const char* v=strchr(argv[i],'='); if(v&&g_ascii_strcasecmp(v+1,"h265")==0) use_h265=TRUE; }
        else if (g_strcmp0(argv[i],"--codec")==0 && i+1<argc){ if (g_ascii_strcasecmp(argv[i+1],"h265")==0) use_h265=TRUE; }
        else if (g_str_has_prefix(argv[i],"--bitrate=")) { const char* v=strchr(argv[i],'='); if(v){ int b=atoi(v+1); if(b>0) bitrate_kbps=b; } }
        else if (g_strcmp0(argv[i],"--bitrate")==0 && i+1<argc){ int b=atoi(argv[i+1]); if(b>0) bitrate_kbps=b; }
        else if (g_str_has_prefix(argv[i],"--workers=")) { const char* v=strchr(argv[i],'='); if(v){ int w=atoi(v+1); if(w>0 && w<=8) num_workers=w; } }
        else if (g_strcmp0(argv[i],"--workers")==0 && i+1<argc){ int w=atoi(argv[i+1]); if(w>0 && w<=8) num_workers=w; }
        else if (g_str_has_prefix(argv[i],"--width=")) { const char* v=strchr(argv[i],'='); if(v){ int w=atoi(v+1); if(w>0) v_width=w; } }
        else if (g_strcmp0(argv[i],"--width")==0 && i+1<argc){ int w=atoi(argv[i+1]); if(w>0) v_width=w; }
        else if (g_str_has_prefix(argv[i],"--height=")) { const char* v=strchr(argv[i],'='); if(v){ int h=atoi(v+1); if(h>0) v_height=h; } }
        else if (g_strcmp0(argv[i],"--height")==0 && i+1<argc){ int h=atoi(argv[i+1]); if(h>0) v_height=h; }
        else if (g_str_has_prefix(argv[i],"--fps=")) { const char* v=strchr(argv[i],'='); if(v){ int f=atoi(v+1); if(f>0) fps=f; } }
        else if (g_strcmp0(argv[i],"--fps")==0 && i+1<argc){ int f=atoi(argv[i+1]); if(f>0) fps=f; }
    }
    g_print("Encoder: %s, target-bitrate: %d kbps, workers: %d, %dx%d@%dfps (OpenCL FPGA Acceleration)\n",
            use_h265 ? "H.265" : "H.264", bitrate_kbps, num_workers, v_width, v_height, fps);

    CustomData d{};
    d.work_q = g_async_queue_new();
    d.num_workers = num_workers;
    
    // Initialize shared OpenCL context first
    if (!initialize_shared_opencl_context(&d.shared_opencl)) {
        g_printerr("Failed to initialize shared OpenCL context\n");
        return -1;
    }
    
    // Allocate worker OpenCL contexts
    d.worker_opencl_contexts = g_new0(WorkerOpenCLContext, num_workers);

    // Capture pipeline (bump queue to 8 for smoothing; add videorate dropper)
    GError *err=NULL;
    gchar *sink_str = g_strdup_printf(
        "v4l2src device=/dev/video0 io-mode=4 ! "
        "video/x-raw,format=NV12,width=%d,height=%d,framerate=60/1 ! "
        "videorate drop-only=true max-rate=%d ! "
        "queue name=q_cam leaky=downstream max-size-buffers=8 max-size-time=0 max-size-bytes=0 ! "
        "appsink name=cv_sink emit-signals=true max-buffers=1 drop=true sync=false",
        v_width, v_height, fps
    );
    GstElement *sink_pipe = gst_parse_launch(sink_str, &err);
    g_free(sink_str);
    if (!sink_pipe) { g_printerr("Create sink pipeline failed: %s\n", err?err->message:"?"); g_clear_error(&err); return -1; }
    d.appsink = gst_bin_get_by_name(GST_BIN(sink_pipe), "cv_sink");
    if (!d.appsink) { g_printerr("Failed to find appsink 'cv_sink'\n"); gst_object_unref(sink_pipe); return -1; }

    // Streaming pipeline (dynamic caps derived from CLI)
    gchar *src_str=NULL;
    if (use_h265) {
        src_str = g_strdup_printf(
            "appsrc name=my_src is-live=true format=GST_FORMAT_TIME do-timestamp=true ! "
            "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/1 ! "
            "queue name=q_after_src leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 ! "
            "omxh265enc name=enc num-slices=8 periodicity-idr=240 cpb-size=500 gdr-mode=horizontal "
            "initial-delay=250 control-rate=low-latency prefetch-buffer=true target-bitrate=%d "
            "gop-mode=low-delay-p ! video/x-h265,alignment=au ! "
            "rtph265pay ! "
            "udpsink buffer-size=60000000 host=192.168.25.69 port=5004 async=false max-lateness=-1 qos-dscp=60",
            v_width, v_height, fps, bitrate_kbps
        );
    } else {
        src_str = g_strdup_printf(
            "appsrc name=my_src is-live=true format=GST_FORMAT_TIME do-timestamp=true ! "
            "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/1 ! "
            "queue name=q_after_src leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 ! "
            "omxh264enc name=enc num-slices=8 periodicity-idr=240 cpb-size=500 gdr-mode=horizontal "
            "initial-delay=250 control-rate=low-latency prefetch-buffer=true target-bitrate=%d "
            "gop-mode=low-delay-p ! video/x-h264,alignment=nal ! "
            "rtph264pay ! "
            "udpsink buffer-size=60000000 host=192.168.25.69 port=5004 async=false max-lateness=-1 qos-dscp=60",
            v_width, v_height, fps, bitrate_kbps
        );
    }
    GstElement *src_pipe = gst_parse_launch(src_str, &err);
    g_free(src_str);
    if (!src_pipe) {
        g_printerr("Create src pipeline failed: %s\n", err?err->message:"?");
        g_clear_error(&err);
        gst_object_unref(sink_pipe);
        return -1;
    }
    d.appsrc = gst_bin_get_by_name(GST_BIN(src_pipe), "my_src");
    if (!d.appsrc) {
        g_printerr("Failed to find appsrc 'my_src'\n");
        gst_object_unref(src_pipe);
        gst_object_unref(sink_pipe);
        return -1;
    }

    // Probes: q_cam.sink, q_cam.src, appsink.sink, q_after_src.src, enc.sink
    {
        GstElement *q_cam = gst_bin_get_by_name(GST_BIN(sink_pipe), "q_cam");
        if (q_cam) {
            if (GstPad *p = gst_element_get_static_pad(q_cam, "sink")) { gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, probe_cam_out, &d, NULL); gst_object_unref(p); }
            if (GstPad *p = gst_element_get_static_pad(q_cam, "src"))  { gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, probe_qcam_out, &d, NULL); gst_object_unref(p); }
            gst_object_unref(q_cam);
        }
    }
    { if (GstPad *p = gst_element_get_static_pad(d.appsink, "sink")) { gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, probe_apps_sink, &d, NULL); gst_object_unref(p); } }
    {
        GstElement *q = gst_bin_get_by_name(GST_BIN(src_pipe), "q_after_src");
        if (q) { if (GstPad *p = gst_element_get_static_pad(q, "src")) { gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, probe_after_appsrc_queue, &d, NULL); gst_object_unref(p); } gst_object_unref(q); }
    }
    {
        GstElement *enc = gst_bin_get_by_name(GST_BIN(src_pipe), "enc");
        if (enc) { if (GstPad *p = gst_element_get_static_pad(enc, "sink")) { gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, probe_encoder_sink, &d, NULL); gst_object_unref(p); } gst_object_unref(enc); }
    }

    // Callback + workers
    g_signal_connect(d.appsink, "new-sample", G_CALLBACK(new_sample_cb), &d);

    // Start multiple worker threads with individual data structures
    d.workers = g_new(GThread*, d.num_workers);
    struct WorkerData {
        CustomData* main_data;
        int worker_id;
    };
    WorkerData* worker_datas = g_new(WorkerData, d.num_workers);
    
    for (int i = 0; i < d.num_workers; i++) {
        worker_datas[i].main_data = &d;
        worker_datas[i].worker_id = i;
        
        gchar *thread_name = g_strdup_printf("opencl-fpga-worker-%d", i);
        d.workers[i] = g_thread_new(thread_name, worker_thread_fn, &worker_datas[i]);
        g_free(thread_name);
    }

    // GLib loop + watches + status timer
    d.loop = g_main_loop_new(NULL, FALSE);
    GstBus *bus_sink = gst_element_get_bus(sink_pipe);
    GstBus *bus_src  = gst_element_get_bus(src_pipe);
    gst_bus_add_watch(bus_sink, bus_cb, &d);
    gst_bus_add_watch(bus_src,  bus_cb, &d);
    g_timeout_add_seconds(2, status_tick, &d);

    // Start & run
    gst_element_set_state(src_pipe,  GST_STATE_PLAYING);
    gst_element_set_state(sink_pipe, GST_STATE_PLAYING);
    g_print("Huiiiiiiiiiiiii (OpenCL FPGA histogram equalization, worker-decoupled). Press Ctrl+C to exit.\n");
    g_main_loop_run(d.loop);

    // Shutdown
    d.stop.store(true, std::memory_order_release);
    // Drain worker queue
    while (GstBuffer *b = (GstBuffer*)g_async_queue_try_pop(d.work_q)) gst_buffer_unref(b);

    // Join all worker threads
    if (d.workers) {
        for (int i = 0; i < d.num_workers; i++) {
            if (d.workers[i]) {
                g_thread_join(d.workers[i]);
            }
        }
        g_free(d.workers);
        d.workers = nullptr;
    }

    // Cleanup
    cleanup_shared_opencl_context(&d.shared_opencl);
    
    if (d.worker_opencl_contexts) {
        g_free(d.worker_opencl_contexts);
        d.worker_opencl_contexts = nullptr;
    }
    
    g_free(worker_datas);

    if (d.work_q) { g_async_queue_unref(d.work_q); d.work_q = nullptr; }

    gst_element_set_state(sink_pipe, GST_STATE_NULL);
    gst_element_set_state(src_pipe,  GST_STATE_NULL);
    gst_object_unref(bus_sink);
    gst_object_unref(bus_src);
    gst_object_unref(d.appsink);
    gst_object_unref(d.appsrc);
    gst_object_unref(sink_pipe);
    gst_object_unref(src_pipe);
    g_main_loop_unref(d.loop);
    return 0;
}