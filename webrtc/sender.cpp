// webrtc_sender.cpp
// Corrected GStreamer WebRTC sender for MPSoC (H.264 / H.265) with WebSocket signaling.
//
// Build (libsoup-2.4):
//   g++ -O2 -std=c++17 webrtc_sender.cpp -o webrtc_sender \
//     $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
//                  gobject-2.0 glib-2.0 json-glib-1.0 libsoup-2.4)
//
// Build (libsoup-3.0):
//   g++ -O2 -std=c++17 webrtc_sender.cpp -o webrtc_sender \
//     $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
//                  gobject-2.0 glib-2.0 json-glib-1.0 libsoup-3.0)
//
// Run:
//   ./webrtc_sender --ws=ws://<CLIENT_PC_IP>:8080 --room=default \
//     --width=1280 --height=720 --fps=30 --codec=h265 --bitrate=200
//
// Notes:
// - --codec can be h264 or h265
// - --bitrate is in kbps
// - Add/change v4l2src device= if needed.

#include <gst/gst.h>
#include <gst/sdp/gstsdpmessage.h>
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>
#include <glib.h>
#include <string>
#include <iostream>
#include <memory>

#include <libsoup/soup.h>  // works for both soup-2.4 and soup-3.x

// --------------------------- Args & App ---------------------------

struct Args {
  std::string ws = "ws://192.168.25.69:8080";
  std::string room = "default";
  int width = 1280;
  int height = 720;
  int fps = 30;
  std::string codec = "h264";   // "h264" or "h265"
  int bitrate_kbps = 200;       // kbps
  std::string device = "/dev/video0";
};

static void parse_args(int argc, char **argv, Args &a) {
  for (int i=1;i<argc;i++) {
    std::string s = argv[i];
    auto next = [&](int &i){ return std::string(argv[++i]); };
    if      (s.rfind("--ws=",0)==0) a.ws = s.substr(5);
    else if (s=="--ws" && i+1<argc) a.ws = next(i);
    else if (s.rfind("--room=",0)==0) a.room = s.substr(7);
    else if (s=="--room" && i+1<argc) a.room = next(i);
    else if (s.rfind("--width=",0)==0) a.width = std::stoi(s.substr(8));
    else if (s=="--width" && i+1<argc) a.width = std::stoi(next(i));
    else if (s.rfind("--height=",0)==0) a.height = std::stoi(s.substr(9));
    else if (s=="--height" && i+1<argc) a.height = std::stoi(next(i));
    else if (s.rfind("--fps=",0)==0) a.fps = std::stoi(s.substr(6));
    else if (s=="--fps" && i+1<argc) a.fps = std::stoi(next(i));
    else if (s.rfind("--codec=",0)==0) a.codec = s.substr(8);
    else if (s=="--codec" && i+1<argc) a.codec = next(i);
    else if (s.rfind("--bitrate=",0)==0) a.bitrate_kbps = std::stoi(s.substr(10));
    else if (s=="--bitrate" && i+1<argc) a.bitrate_kbps = std::stoi(next(i));
    else if (s.rfind("--device=",0)==0) a.device = s.substr(9);
    else if (s=="--device" && i+1<argc) a.device = next(i);
  }
}

struct App {
  Args args;
  GMainLoop *loop = nullptr;

  GstElement *pipeline = nullptr;
  GstElement *webrtc = nullptr;

  SoupSession *soup = nullptr;
  SoupWebsocketConnection *ws = nullptr;

  bool have_sdp_answer = false;
};

// --------------------------- Helpers ---------------------------

static void send_json(App* app, JsonBuilder *b) {
  if (!app || !app->ws || soup_websocket_connection_get_state(app->ws) != SOUP_WEBSOCKET_STATE_OPEN) {
    return;
  }

  JsonGenerator *gen = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  
  gsize data_len = 0;
  gchar *txt = json_generator_to_data(gen, &data_len);

  soup_websocket_connection_send_text(app->ws, txt);
  
  g_free(txt);
  g_object_unref(gen);
  json_node_free(root);
}

static std::string pipeline_desc(const Args& a) {
  const bool h265 = (a.codec == "h265" || a.codec == "H265");
  std::string enc, parse, pay, paycaps;

  if (h265) {
    // Use fallback to software encoder if hardware isn't available
    enc = "omxh265enc target-bitrate=" + std::to_string(a.bitrate_kbps * 1000) +
          " control-rate=low-latency gop-mode=low-delay-p periodicity-idr=240 ! " +
          "fallback. ( fallback. queue ! x265enc bitrate=" + std::to_string(a.bitrate_kbps) +
          " tune=zerolatency speed-preset=ultrafast key-int-max=240 )";
    parse = "h265parse config-interval=1";
    pay  = "rtph265pay config-interval=1 pt=96 aggregate-mode=zero-latency";
    paycaps = "application/x-rtp,media=video,encoding-name=H265,payload=96,clock-rate=90000";
  } else {
    // Use fallback to software encoder if hardware isn't available
    enc = "omxh264enc target-bitrate=" + std::to_string(a.bitrate_kbps * 1000) +
          " control-rate=low-latency gop-mode=low-delay-p periodicity-idr=240 ! " +
          "fallback. ( fallback. queue ! x264enc bitrate=" + std::to_string(a.bitrate_kbps) +
          " tune=zerolatency speed-preset=ultrafast key-int-max=240 )";
    parse = "h264parse config-interval=1";
    pay  = "rtph264pay config-interval=1 pt=96 aggregate-mode=zero-latency";
    paycaps = "application/x-rtp,media=video,encoding-name=H264,packetization-mode=1,payload=96,clock-rate=90000";
  }

  char caps[512];
  std::snprintf(caps, sizeof(caps),
    "video/x-raw,format=(string)NV12,width=%d,height=%d,framerate=%d/1",
    a.width, a.height, a.fps);

  std::string desc =
    "v4l2src device=" + a.device + " io-mode=2 ! " + caps + 
    " ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! " +
    enc + " ! " + parse + " ! " + pay + " ! " + paycaps +
    " ! webrtcbin name=sendrecv bundle-policy=max-bundle stun-server=stun://stun.l.google.com:19302";

  return desc;
}

// --------------------------- GStreamer callbacks ---------------------------

static gboolean bus_cb(GstBus*, GstMessage *msg, gpointer user_data) {
  App *app = (App*)user_data;
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = nullptr; gchar *dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      g_printerr("ERROR: %s (%s)\n", err->message, dbg ? dbg : "no-debug");
      g_error_free(err); g_free(dbg);
      if (app->loop) g_main_loop_quit(app->loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError *err = nullptr; gchar *dbg = nullptr;
      gst_message_parse_warning(msg, &err, &dbg);
      g_printerr("WARN: %s (%s)\n", err->message, dbg ? dbg : "no-debug");
      g_error_free(err); g_free(dbg);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(app->pipeline)) {
        GstState oldst, newst, pen;
        gst_message_parse_state_changed(msg, &oldst, &newst, &pen);
        g_print("Pipeline state: %s -> %s\n", 
                gst_element_state_get_name(oldst),
                gst_element_state_get_name(newst));
      }
      break;
    }
    case GST_MESSAGE_EOS:
      g_print("End-of-stream reached\n");
      if (app->loop) g_main_loop_quit(app->loop);
      break;
    default: break;
  }
  return TRUE;
}

static void on_offer_created(GstPromise *promise, gpointer user_data) {
  App *app = (App*)user_data;
  
  if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) {
    g_printerr("Failed to create offer\n");
    gst_promise_unref(promise);
    return;
  }
  
  const GstStructure *reply = gst_promise_get_reply(promise);
  if (!reply) {
    g_printerr("No reply from create-offer promise\n");
    gst_promise_unref(promise);
    return;
  }
  
  GstWebRTCSessionDescription *offer = nullptr;
  if (!gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr)) {
    g_printerr("Failed to get offer from reply\n");
    gst_promise_unref(promise);
    return;
  }
  
  gst_promise_unref(promise);

  // Create a promise for setting local description
  GstPromise *local_desc_promise = gst_promise_new();
  g_signal_emit_by_name(app->webrtc, "set-local-description", offer, local_desc_promise);
  gst_promise_wait(local_desc_promise);
  gst_promise_unref(local_desc_promise);

  gchar *sdp_txt = gst_sdp_message_as_text(offer->sdp);

  // Send SDP offer
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "offer");
  json_builder_set_member_name(b, "room"); json_builder_add_string_value(b, app->args.room.c_str());
  json_builder_set_member_name(b, "sdp");  json_builder_add_string_value(b, sdp_txt);
  json_builder_end_object(b);
  send_json(app, b);
  g_object_unref(b);

  g_print("Sent SDP offer\n");
  
  g_free(sdp_txt);
  gst_webrtc_session_description_free(offer);
}

static void on_negotiation_needed(GstElement *, gpointer user_data) {
  App *app = (App*)user_data;
  g_print("Negotiation needed, creating offer...\n");
  GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, app, NULL);
  g_signal_emit_by_name(app->webrtc, "create-offer", NULL, promise);
}

static void on_ice_candidate(GstElement *, guint mline, gchar *candidate, gpointer user_data) {
  App *app = (App*)user_data;
  g_print("ICE candidate: %s\n", candidate);
  
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "ice");
  json_builder_set_member_name(b, "room"); json_builder_add_string_value(b, app->args.room.c_str());
  json_builder_set_member_name(b, "candidate"); json_builder_add_string_value(b, candidate);
  json_builder_set_member_name(b, "sdpMLineIndex"); json_builder_add_int_value(b, (int)mline);
  json_builder_end_object(b);
  send_json(app, b);
  g_object_unref(b);
}

static void on_ice_gathering_state_notify(GstElement *, GParamSpec *, gpointer user_data) {
  App *app = (App*)user_data;
  GstWebRTCICEGatheringState ice_gather_state;
  g_object_get(app->webrtc, "ice-gathering-state", &ice_gather_state, NULL);

  const char* state_str = "";
  switch (ice_gather_state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW: state_str = "new"; break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING: state_str = "gathering"; break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE: state_str = "complete"; break;
  }
  g_print("ICE gathering state changed to: %s\n", state_str);
}

// --------------------------- WebSocket (signaling) ---------------------------

static void ws_on_message(SoupWebsocketConnection *, gint type, GBytes *msg, gpointer user_data) {
  App *app = (App*)user_data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) return;

  gsize sz = 0;
  const gchar *data = (const gchar*)g_bytes_get_data(msg, &sz);
  g_print("Received WebSocket message: %.*s\n", (int)sz, data);

  JsonParser *parser = json_parser_new();
  GError *error = nullptr;
  if (!json_parser_load_from_data(parser, data, sz, &error)) {
    g_printerr("Failed to parse JSON: %s\n", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return;
  }

  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  if (!obj || !json_object_has_member(obj, "type")) {
    g_printerr("Invalid message format\n");
    g_object_unref(parser);
    return;
  }
  
  const gchar *mtype = json_object_get_string_member(obj, "type");

  if (g_strcmp0(mtype, "answer") == 0) {
    if (!json_object_has_member(obj, "sdp")) {
      g_printerr("Answer message missing SDP\n");
      g_object_unref(parser);
      return;
    }
    
    const gchar *sdp = json_object_get_string_member(obj, "sdp");
    GstSDPMessage *sdpmsg = nullptr;
    
    if (gst_sdp_message_new(&sdpmsg) == GST_SDP_OK &&
        gst_sdp_message_parse_buffer((guint8*)sdp, (guint)strlen(sdp), sdpmsg) == GST_SDP_OK) {

      GstWebRTCSessionDescription *answer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdpmsg);
      
      GstPromise *promise = gst_promise_new();
      g_signal_emit_by_name(app->webrtc, "set-remote-description", answer, promise);
      gst_promise_wait(promise);
      gst_promise_unref(promise);
      
      gst_webrtc_session_description_free(answer);
      app->have_sdp_answer = true;
      g_print("Set remote SDP answer\n");
    } else {
      g_printerr("Failed to parse SDP answer\n");
      if (sdpmsg) gst_sdp_message_free(sdpmsg);
    }
  } else if (g_strcmp0(mtype, "ice") == 0) {
    if (!json_object_has_member(obj, "candidate") || !json_object_has_member(obj, "sdpMLineIndex")) {
      g_printerr("ICE message missing required fields\n");
      g_object_unref(parser);
      return;
    }
    
    const gchar *candidate = json_object_get_string_member(obj, "candidate");
    int sdpmlineindex = json_object_get_int_member(obj, "sdpMLineIndex");
    g_print("Adding remote ICE candidate: %s (line %d)\n", candidate, sdpmlineindex);
    g_signal_emit_by_name(app->webrtc, "add-ice-candidate", sdpmlineindex, candidate);
  } else if (g_strcmp0(mtype, "ready") == 0) {
    g_print("Viewer joined (ready). Negotiation will proceed.\n");
  }

  g_object_unref(parser);
}

static void ws_on_closed(SoupWebsocketConnection*, gpointer user_data) {
  App *app = (App*)user_data;
  g_print("WebSocket closed\n");
  if (app->loop) g_main_loop_quit(app->loop);
}

static void ws_on_error(SoupWebsocketConnection*, GError* error, gpointer user_data) {
  App *app = (App*)user_data;
  g_printerr("WebSocket error: %s\n", error->message);
  if (app->loop) g_main_loop_quit(app->loop);
}

static void ws_connected(SoupSession*, GAsyncResult *res, gpointer user_data) {
  App *app = (App*)user_data;
  GError *err = nullptr;
  app->ws = soup_session_websocket_connect_finish(app->soup, res, &err);
  if (err) {
    g_printerr("WebSocket connect failed: %s\n", err->message);
    g_error_free(err);
    if (app->loop) g_main_loop_quit(app->loop);
    return;
  }

  g_signal_connect(app->ws, "message", G_CALLBACK(ws_on_message), app);
  g_signal_connect(app->ws, "closed",  G_CALLBACK(ws_on_closed),  app);
  g_signal_connect(app->ws, "error",   G_CALLBACK(ws_on_error),   app);

  // Send join {type, room, clientType}
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");       json_builder_add_string_value(b, "join");
  json_builder_set_member_name(b, "room");       json_builder_add_string_value(b, app->args.room.c_str());
  json_builder_set_member_name(b, "clientType"); json_builder_add_string_value(b, "sender");
  json_builder_end_object(b);
  send_json(app, b);
  g_object_unref(b);

  g_print("WebSocket connected to %s\n", app->args.ws.c_str());
}

// --------------------------- main ---------------------------

int main(int argc, char **argv) {
  gst_init(&argc, &argv);

  App app;
  parse_args(argc, argv, app.args);

  if (app.args.codec != "h264" && app.args.codec != "h265") {
    g_printerr("--codec must be h264 or h265 (got %s)\n", app.args.codec.c_str());
    return 1;
  }

  g_print("Starting WebRTC sender with:\n");
  g_print("  Resolution: %dx%d @ %dfps\n", app.args.width, app.args.height, app.args.fps);
  g_print("  Codec: %s @ %d kbps\n", app.args.codec.c_str(), app.args.bitrate_kbps);
  g_print("  Device: %s\n", app.args.device.c_str());
  g_print("  WebSocket: %s\n", app.args.ws.c_str());
  g_print("  Room: %s\n", app.args.room.c_str());

  // Build pipeline
  GError *err = nullptr;
  std::string desc = pipeline_desc(app.args);
  g_print("Pipeline: %s\n", desc.c_str());
  
  app.pipeline = gst_parse_launch(desc.c_str(), &err);
  if (!app.pipeline) {
    g_printerr("Pipeline error: %s\n", err ? err->message : "(unknown)");
    if (err) g_error_free(err);
    return 1;
  }

  app.webrtc = gst_bin_get_by_name(GST_BIN(app.pipeline), "sendrecv");
  if (!app.webrtc) {
    g_printerr("Failed to get webrtc element from pipeline\n");
    g_object_unref(app.pipeline);
    return 1;
  }

  g_signal_connect(app.webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), &app);
  g_signal_connect(app.webrtc, "on-ice-candidate",      G_CALLBACK(on_ice_candidate),      &app);
  g_signal_connect(app.webrtc, "notify::ice-gathering-state", G_CALLBACK(on_ice_gathering_state_notify), &app);

  // Bus watch
  GstBus *bus = gst_element_get_bus(app.pipeline);
  gst_bus_add_watch(bus, bus_cb, &app);
  gst_object_unref(bus);

  // WebSocket connect (libsoup-2.4 vs 3.x)
  app.soup = soup_session_new();
#if defined(SOUP_MAJOR_VERSION) && (SOUP_MAJOR_VERSION >= 3)
  // libsoup-3.x
  GError *uri_error = nullptr;
  GUri *uri = g_uri_parse(app.args.ws.c_str(), G_URI_FLAGS_NONE, &uri_error);
  if (!uri) {
    g_printerr("Invalid WebSocket URI: %s\n", uri_error->message);
    g_error_free(uri_error);
    return 1;
  }
  SoupMessage *msg = soup_message_new_from_uri("GET", uri);
  soup_session_websocket_connect_async(app.soup,
      msg, /*origin*/ nullptr, /*protocols*/ nullptr, /*cancellable*/ nullptr,
      (GAsyncReadyCallback)ws_connected, &app);
  g_object_unref(msg);
  g_uri_unref(uri);
#else
  // libsoup-2.4
  SoupMessage *msg = soup_message_new("GET", app.args.ws.c_str());
  if (!msg) {
    g_printerr("Failed to create WebSocket message for %s\n", app.args.ws.c_str());
    return 1;
  }
  soup_session_websocket_connect_async(app.soup,
      msg, /*origin*/ nullptr, /*protocols*/ nullptr, /*cancellable*/ nullptr,
      (GAsyncReadyCallback)ws_connected, &app);
  g_object_unref(msg);
#endif

  // Go!
  app.loop = g_main_loop_new(NULL, FALSE);
  g_print("Setting pipeline to PLAYING...\n");
  GstStateChangeReturn ret = gst_element_set_state(app.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to set pipeline to PLAYING state\n");
    return 1;
  }
  
  g_print("Starting main loop...\n");
  g_main_loop_run(app.loop);

  // Teardown
  g_print("Shutting down...\n");
  gst_element_set_state(app.pipeline, GST_STATE_NULL);
  if (app.webrtc)   g_object_unref(app.webrtc);
  if (app.pipeline) g_object_unref(app.pipeline);
  if (app.ws)       g_object_unref(app.ws);
  if (app.soup)     g_object_unref(app.soup);
  if (app.loop)     g_main_loop_unref(app.loop);

  return 0;
}