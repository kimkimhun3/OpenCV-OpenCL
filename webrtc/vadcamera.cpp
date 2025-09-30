#define GST_USE_UNSTABLE_API

#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <iostream>
#include <getopt.h>  // For getopt_long and struct option

// Configuration structure
struct Config {
    gchar *codec;
    gint bitrate;
    gint fps;
    gint width;
    gint height;
    gchar *device;
};

// Global variables
static GstElement *pipeline = NULL;
static GstElement *webrtc = NULL;
static SoupWebsocketConnection *ws_conn = NULL;
static GMainLoop *loop = NULL;
static gchar *peer_id = NULL;
static gchar *my_id = NULL;
static struct Config config;

// Signaling server details (update with your PC's IP)
static const gchar *server_url = "ws://192.168.25.69:8080";  // CHANGE THIS TO YOUR PC IP

// Function declarations
static void on_offer_created(GstPromise *promise, gpointer user_data);
static void on_negotiation_needed(GstElement *element, gpointer user_data);
static void on_ice_candidate(GstElement *webrtc, guint mlineindex, gchar *candidate, gpointer user_data);
static void send_ice_candidate_message(guint mlineindex, const gchar *candidate);
static void on_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data);

// Send JSON message via WebSocket
static void send_json_message(JsonObject *msg) {
    if (!ws_conn) {
        g_printerr("WebSocket not connected\n");
        return;
    }

    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, msg);
    
    gchar *text = json_to_string(root, FALSE);
    g_print("Sending: %s\n", text);
    
    soup_websocket_connection_send_text(ws_conn, text);
    
    g_free(text);
    json_node_free(root);
}

// Handle incoming WebSocket messages
static void on_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                       GBytes *message, gpointer user_data) {
    if (type != SOUP_WEBSOCKET_DATA_TEXT) {
        return;
    }

    gsize size;
    const gchar *data = (const gchar *)g_bytes_get_data(message, &size);
    gchar *text = g_strndup(data, size);
    
    g_print("Received: %s\n", text);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, NULL)) {
        g_printerr("Failed to parse JSON\n");
        g_free(text);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *object = json_node_get_object(root);
    const gchar *msg_type = json_object_get_string_member(object, "type");

    if (g_strcmp0(msg_type, "registered") == 0) {
        my_id = g_strdup(json_object_get_string_member(object, "id"));
        g_print("Registered with ID: %s\n", my_id);
        
    } else if (g_strcmp0(msg_type, "answer") == 0) {
        const gchar *sdp_text = json_object_get_string_member(object, "sdp");
        peer_id = g_strdup(json_object_get_string_member(object, "from"));
        
        g_print("Received answer from: %s\n", peer_id);

        GstSDPMessage *sdp;
        gst_sdp_message_new(&sdp);
        gst_sdp_message_parse_buffer((guint8 *)sdp_text, strlen(sdp_text), sdp);

        GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(
            GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        
        GstPromise *promise = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        
        gst_webrtc_session_description_free(answer);
        
    } else if (g_strcmp0(msg_type, "ice-candidate") == 0) {
        JsonObject *candidate_obj = json_object_get_object_member(object, "candidate");
        const gchar *candidate_str = json_object_get_string_member(candidate_obj, "candidate");
        
        // Handle empty candidate (end-of-candidates signal)
        if (!candidate_str || strlen(candidate_str) == 0) {
            g_print("Received end-of-candidates signal, ignoring\n");
            g_free(text);
            g_object_unref(parser);
            return;
        }
        
        guint sdp_mline_index = json_object_get_int_member(candidate_obj, "sdpMLineIndex");
        
        g_print("Received ICE candidate: %s\n", candidate_str);
        g_signal_emit_by_name(webrtc, "add-ice-candidate", sdp_mline_index, candidate_str);
    }

    g_free(text);
    g_object_unref(parser);
}

// Send ICE candidate via signaling
static void send_ice_candidate_message(guint mlineindex, const gchar *candidate) {
    // Determine the correct mid based on mlineindex
    const gchar *mid = NULL;
    if (mlineindex == 0) {
        mid = "video0";
    } else if (mlineindex == 1) {
        mid = "audio1";
    } else {
        mid = "video0";  // fallback
    }

    JsonObject *ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);
    json_object_set_string_member(ice, "sdpMid", mid);

    JsonObject *msg = json_object_new();
    json_object_set_string_member(msg, "type", "ice-candidate");
    json_object_set_object_member(msg, "candidate", ice);
    if (peer_id) {
        json_object_set_string_member(msg, "to", peer_id);
    }

    send_json_message(msg);
    json_object_unref(msg);
}

// Handle ICE candidate generation
static void on_ice_candidate(GstElement *webrtc, guint mlineindex,
                             gchar *candidate, gpointer user_data) {
    g_print("Generated ICE candidate: %s\n", candidate);
    send_ice_candidate_message(mlineindex, candidate);
}

// Handle offer creation
static void on_offer_created(GstPromise *promise, gpointer user_data) {
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    g_print("Offer created, setting local description\n");
    
    promise = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", offer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    // Send offer via signaling
    gchar *sdp_text = gst_sdp_message_as_text(offer->sdp);
    
    JsonObject *msg = json_object_new();
    json_object_set_string_member(msg, "type", "offer");
    json_object_set_string_member(msg, "sdp", sdp_text);

    send_json_message(msg);
    
    g_free(sdp_text);
    json_object_unref(msg);
    gst_webrtc_session_description_free(offer);
}

// Handle negotiation needed
static void on_negotiation_needed(GstElement *element, gpointer user_data) {
    g_print("Negotiation needed, creating offer\n");
    
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

// Handle incoming stream (shouldn't happen in this case, but good to have)
static void on_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data) {
    g_print("Received incoming stream\n");
}

// Handle ICE gathering state changes
static void on_ice_gathering_state_notify(GstElement *webrtc, GParamSpec *pspec, gpointer user_data) {
    GstWebRTCICEGatheringState ice_gather_state;
    g_object_get(webrtc, "ice-gathering-state", &ice_gather_state, NULL);
    
    const gchar *state_str = NULL;
    switch (ice_gather_state) {
        case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
            state_str = "new";
            break;
        case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
            state_str = "gathering";
            break;
        case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
            state_str = "complete";
            break;
        default:
            state_str = "unknown";
    }
    
    g_print("ICE gathering state changed to: %s\n", state_str);
}

// Handle ICE connection state changes
static void on_ice_connection_state_notify(GstElement *webrtc, GParamSpec *pspec, gpointer user_data) {
    GstWebRTCICEConnectionState ice_conn_state;
    g_object_get(webrtc, "ice-connection-state", &ice_conn_state, NULL);
    
    const gchar *state_str = NULL;
    switch (ice_conn_state) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:
            state_str = "new";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:
            state_str = "checking";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
            state_str = "connected";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:
            state_str = "completed";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
            state_str = "failed";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
            state_str = "disconnected";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:
            state_str = "closed";
            break;
        default:
            state_str = "unknown";
    }
    
    g_print("ICE connection state changed to: %s\n", state_str);
}

// WebSocket connection established
static void on_websocket_connected(GObject *session, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    ws_conn = soup_session_websocket_connect_finish(SOUP_SESSION(session), res, &error);
    
    if (error) {
        g_printerr("WebSocket connection failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }

    g_print("WebSocket connected to signaling server\n");
    
    g_signal_connect(ws_conn, "message", G_CALLBACK(on_message), NULL);
    
    g_signal_connect(ws_conn, "closed", G_CALLBACK(+[](SoupWebsocketConnection *conn, gpointer data) {
        g_print("WebSocket closed\n");
        g_main_loop_quit((GMainLoop*)data);
    }), loop);
}

// Bus message handler
static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(message, &err, &debug);
            g_printerr("Error: %s\n", err->message);
            g_printerr("Debug: %s\n", debug);
            g_error_free(err);
            g_free(debug);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err;
            gchar *debug;
            gst_message_parse_warning(message, &err, &debug);
            g_printerr("Warning: %s\n", err->message);
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(loop);
            break;
        default:
            break;
    }
    return TRUE;
}

// Print usage information
static void print_usage(const char *prog_name) {
    g_print("Usage: %s [OPTIONS]\n", prog_name);
    g_print("\nOptions:\n");
    g_print("  --codec=CODEC       Video codec: h264 or h265 (default: h264)\n");
    g_print("  --bitrate=KBPS      Video bitrate in kbps (default: 2000)\n");
    g_print("  --fps=FPS           Framerate (default: 30)\n");
    g_print("  --width=WIDTH       Video width (default: 1280)\n");
    g_print("  --height=HEIGHT     Video height (default: 720)\n");
    g_print("  --device=PATH       Camera device path (default: /dev/video0)\n");
    g_print("  --help              Show this help message\n");
    g_print("\nExamples:\n");
    g_print("  %s --codec=h264 --bitrate=5000 --fps=30\n", prog_name);
    g_print("  %s --codec=h265 --bitrate=3000 --fps=25 --width=1920 --height=1080\n", prog_name);
    g_print("\n");
}

// Parse command line arguments
static gboolean parse_arguments(int argc, char *argv[]) {
    // Set defaults
    config.codec = g_strdup("h264");
    config.bitrate = 2000;
    config.fps = 30;
    config.width = 1280;
    config.height = 720;
    config.device = g_strdup("/dev/video0");

    struct option long_options[] = {
        {"codec",    required_argument, 0, 'c'},
        {"bitrate",  required_argument, 0, 'b'},
        {"fps",      required_argument, 0, 'f'},
        {"width",    required_argument, 0, 'w'},
        {"height",   required_argument, 0, 'H'},
        {"device",   required_argument, 0, 'd'},
        {"help",     no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;

    while ((c = getopt_long(argc, argv, "c:b:f:w:H:d:?", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                g_free(config.codec);
                config.codec = g_strdup(optarg);
                if (g_strcmp0(config.codec, "h264") != 0 && g_strcmp0(config.codec, "h265") != 0) {
                    g_printerr("Error: codec must be 'h264' or 'h265'\n");
                    return FALSE;
                }
                break;
            case 'b':
                config.bitrate = atoi(optarg);
                if (config.bitrate <= 0) {
                    g_printerr("Error: bitrate must be positive\n");
                    return FALSE;
                }
                break;
            case 'f':
                config.fps = atoi(optarg);
                if (config.fps <= 0 || config.fps > 120) {
                    g_printerr("Error: fps must be between 1 and 120\n");
                    return FALSE;
                }
                break;
            case 'w':
                config.width = atoi(optarg);
                if (config.width <= 0) {
                    g_printerr("Error: width must be positive\n");
                    return FALSE;
                }
                break;
            case 'H':
                config.height = atoi(optarg);
                if (config.height <= 0) {
                    g_printerr("Error: height must be positive\n");
                    return FALSE;
                }
                break;
            case 'd':
                g_free(config.device);
                config.device = g_strdup(optarg);
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return FALSE;
        }
    }

    return TRUE;
}

// Build GStreamer pipeline based on configuration
static std::string build_pipeline_string() {
    std::string pipeline_str;
    
    // Determine encoder and payloader based on codec
    const char *encoder;
    const char *parser;
    const char *payloader;
    const char *encoding_name;
    int payload;

    if (g_strcmp0(config.codec, "h265") == 0) {
        encoder = "omxh265enc";
        parser = "h265parse";
        payloader = "rtph265pay";
        encoding_name = "H265";
        payload = 96;
    } else {
        encoder = "omxh264enc";
        parser = "h264parse";
        payloader = "rtph264pay";
        encoding_name = "H264";
        payload = 96;
    }

    // Build the pipeline
    char pipeline_buf[2048];
    snprintf(pipeline_buf, sizeof(pipeline_buf),
        "webrtcbin name=webrtcbin bundle-policy=max-bundle latency=100 "
        "stun-server=stun://stun.l.google.com:19302 "
        "v4l2src device=%s ! "
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
        "videoconvert ! "
        "queue max-size-buffers=3 leaky=downstream ! "
        "%s target-bitrate=%d control-rate=2 ! "
        "video/x-%s,profile=%s ! "
        "%s config-interval=1 ! "
        "%s config-interval=1 ! "
        "application/x-rtp,media=video,encoding-name=%s,payload=%d ! "
        "webrtcbin. "
        "audiotestsrc is-live=true wave=silence ! "
        "audioconvert ! "
        "audioresample ! "
        "queue ! "
        "opusenc ! "
        "rtpopuspay ! "
        "application/x-rtp,media=audio,encoding-name=OPUS,payload=97 ! "
        "webrtcbin.",
        config.device,
        config.width,
        config.height,
        config.fps,
        encoder,
        config.bitrate * 1000,  // Convert kbps to bps
        config.codec,
        (g_strcmp0(config.codec, "h265") == 0) ? "main" : "baseline",
        parser,
        payloader,
        encoding_name,
        payload
    );

    pipeline_str = pipeline_buf;
    
    g_print("\n=== Configuration ===\n");
    g_print("Codec:      %s\n", config.codec);
    g_print("Resolution: %dx%d\n", config.width, config.height);
    g_print("Framerate:  %d fps\n", config.fps);
    g_print("Bitrate:    %d Mbps\n", config.bitrate);
    g_print("Device:     %s\n", config.device);
    g_print("====================\n\n");

    return pipeline_str;
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    // Parse command line arguments
    if (!parse_arguments(argc, argv)) {
        return -1;
    }

    loop = g_main_loop_new(NULL, FALSE);

    // Build pipeline based on configuration
    std::string pipeline_str = build_pipeline_string();

    GError *error = NULL;
    pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    
    if (error) {
        g_printerr("Failed to create pipeline: %s\n", error->message);
        g_error_free(error);
        g_free(config.codec);
        g_free(config.device);
        return -1;
    }

    // Get webrtcbin element
    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtcbin");
    g_assert(webrtc != NULL);

    // Connect signals
    g_signal_connect(webrtc, "on-negotiation-needed", 
                     G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(webrtc, "on-ice-candidate", 
                     G_CALLBACK(on_ice_candidate), NULL);
    g_signal_connect(webrtc, "pad-added", 
                     G_CALLBACK(on_incoming_stream), NULL);
    g_signal_connect(webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(on_ice_gathering_state_notify), NULL);
    g_signal_connect(webrtc, "notify::ice-connection-state",
                     G_CALLBACK(on_ice_connection_state_notify), NULL);

    // Set up bus watch
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    // Connect to signaling server
    g_print("Connecting to signaling server: %s\n", server_url);
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", server_url);
    
    soup_session_websocket_connect_async(session, msg, NULL, NULL, NULL, 
                                         on_websocket_connected, NULL);

    // Start pipeline
    g_print("Starting pipeline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Run main loop
    g_main_loop_run(loop);

    // Cleanup
    g_print("Cleaning up...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    
    if (ws_conn) {
        soup_websocket_connection_close(ws_conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
        g_object_unref(ws_conn);
    }
    
    g_object_unref(session);
    g_main_loop_unref(loop);
    
    g_free(my_id);
    g_free(peer_id);
    g_free(config.codec);
    g_free(config.device);

    return 0;
}