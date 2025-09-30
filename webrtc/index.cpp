#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>        // libsoup-2.4
#include <json-glib/json-glib.h>

#include <string>
#include <sstream>
#include <iostream>
#include <cstring>

enum class Codec { H264, H265 };

struct App {
  // GStreamer
  GMainLoop* loop = nullptr;
  GstElement* pipeline = nullptr;
  GstElement* webrtc = nullptr;

  // Signaling (libsoup 2.4)
  SoupSession* soup = nullptr;
  SoupMessage* ws_msg = nullptr;
  SoupWebsocketConnection* ws_conn = nullptr;

  // Config
  std::string ws_url = "ws://192.168.25.69:8080";
  std::string room   = "default";
  std::string device = "/dev/video0";
  int width = 1280;
  int height = 720;
  int fps = 30;
  int bitrate_kbps = 5000;
  Codec codec = Codec::H264; // default

  // State
  bool ws_ready = false;
  bool offer_sent = false;
};

static void safe_ws_send(App* app, const char* json_str) {
  if (!app->ws_conn) return;
  if (soup_websocket_connection_get_state(app->ws_conn) != SOUP_WEBSOCKET_STATE_OPEN) return;
  soup_websocket_connection_send_text(app->ws_conn, json_str);
  g_print("[ws->] %s\n", json_str);
}

static void send_json_obj(App* app, JsonBuilder* b) {
  JsonNode* root = json_builder_get_root(b);
  gchar* text = json_to_string(root, FALSE);
  safe_ws_send(app, text);
  g_free(text);
  json_node_free(root);
  g_object_unref(b);
}

static void send_join(App* app) {
  JsonBuilder* b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");       json_builder_add_string_value(b, "join");
  json_builder_set_member_name(b, "room");       json_builder_add_string_value(b, app->room.c_str());
  json_builder_set_member_name(b, "clientType"); json_builder_add_string_value(b, "sender");
  json_builder_end_object(b);
  send_json_obj(app, b);
}

static void send_ice(App* app, guint mline, const gchar* candidate) {
  // What your server/viewer already uses
  {
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");            json_builder_add_string_value(b, "ice-candidate");
    json_builder_set_member_name(b, "candidate");       json_builder_add_string_value(b, candidate);
    json_builder_set_member_name(b, "sdpMLineIndex");   json_builder_add_int_value(b, (gint)mline);
    json_builder_end_object(b);
    send_json_obj(app, b);
  }
  // Also send as "ice" for maximum interop (harmless duplicate)
  {
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");            json_builder_add_string_value(b, "ice");
    json_builder_set_member_name(b, "candidate");       json_builder_add_string_value(b, candidate);
    json_builder_set_member_name(b, "sdpMLineIndex");   json_builder_add_int_value(b, (gint)mline);
    json_builder_end_object(b);
    send_json_obj(app, b);
  }
}

static void on_ice_candidate(GstElement*, guint mline, gchar* candidate, gpointer user_data) {
  App* app = (App*)user_data;
  if (!app->ws_ready) return;
  send_ice(app, mline, candidate);
}

static void on_offer_created(GstPromise* promise, gpointer user_data) {
  App* app = (App*)user_data;
  const GstStructure* reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription* offer = nullptr;
  gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
  gst_promise_unref(promise);

  g_signal_emit_by_name(app->webrtc, "set-local-description", offer, NULL);

  gchar* sdp_str = gst_sdp_message_as_text(offer->sdp);
  JsonBuilder* b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");    json_builder_add_string_value(b, "offer");
  json_builder_set_member_name(b, "sdp");     json_builder_add_string_value(b, sdp_str);
  json_builder_set_member_name(b, "sdpType"); json_builder_add_string_value(b, "offer");
  json_builder_end_object(b);
  send_json_obj(app, b);

  g_free(sdp_str);
  gst_webrtc_session_description_free(offer);
  app->offer_sent = true;
}

static void create_and_send_offer(App* app) {
  if (!app->ws_ready || app->offer_sent) return;
  GstPromise* promise = gst_promise_new_with_change_func(on_offer_created, app, NULL);
  g_signal_emit_by_name(app->webrtc, "create-offer", NULL, promise);
}

static void on_negotiation_needed(GstElement*, gpointer user_data) {
  App* app = (App*)user_data;
  g_print("on-negotiation-needed\n");
  if (!app->ws_ready) {
    g_print("WS not ready yet; will offer after join.\n");
    return;
  }
  create_and_send_offer(app);
}

static bool json_member_is_string(JsonObject* obj, const char* name) {
  return json_object_has_member(obj, name) &&
         JSON_NODE_HOLDS_VALUE(json_object_get_member(obj, name)) &&
         json_node_get_value_type(json_object_get_member(obj, name)) == G_TYPE_STRING;
}

static void handle_answer(App* app, JsonObject* obj) {
  // Accept either: {sdp:"<string>"} or {sdp:{type:"answer", sdp:"<string>"}}
  const char* sdp_text = nullptr;
  if (json_member_is_string(obj, "sdp")) {
    sdp_text = json_object_get_string_member(obj, "sdp");
  } else if (json_object_has_member(obj, "sdp") &&
             JSON_NODE_HOLDS_OBJECT(json_object_get_member(obj, "sdp"))) {
    JsonObject* sdp_obj = json_node_get_object(json_object_get_member(obj, "sdp"));
    if (json_member_is_string(sdp_obj, "sdp")) {
      sdp_text = json_object_get_string_member(sdp_obj, "sdp");
    }
  }
  if (!sdp_text) {
    g_printerr("Answer missing 'sdp'\n");
    return;
  }

  GstSDPMessage* sdp;
  if (gst_sdp_message_new(&sdp) != GST_SDP_OK) return;
  if (gst_sdp_message_parse_buffer((guint8*)sdp_text, strlen(sdp_text), sdp) != GST_SDP_OK) {
    g_printerr("Failed to parse answer SDP\n");
    gst_sdp_message_free(sdp);
    return;
  }
  GstWebRTCSessionDescription* answer =
      gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
  g_signal_emit_by_name(app->webrtc, "set-remote-description", answer, NULL);
  gst_webrtc_session_description_free(answer);
  g_print("Remote description (answer) set.\n");
}

static void handle_ice_from_peer(App* app, JsonObject* obj) {
  const char* cand = nullptr;
  int mline = 0;
  if (json_member_is_string(obj, "candidate"))
    cand = json_object_get_string_member(obj, "candidate");
  if (json_object_has_member(obj, "sdpMLineIndex") &&
      JSON_NODE_HOLDS_VALUE(json_object_get_member(obj, "sdpMLineIndex"))) {
    mline = json_object_get_int_member(obj, "sdpMLineIndex");
  }
  if (!cand) return;
  g_signal_emit_by_name(app->webrtc, "add-ice-candidate", mline, cand);
}

static void on_ws_message(SoupWebsocketConnection*,
                          SoupWebsocketDataType type,
                          GBytes* message,
                          gpointer user_data) {
  App* app = (App*)user_data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) return;

  gsize sz = 0;
  const char* data = (const char*)g_bytes_get_data(message, &sz);
  g_print("[ws<-] %.*s\n", (int)sz, data);

  JsonParser* p = json_parser_new();
  GError* jerr = nullptr;
  if (!json_parser_load_from_data(p, data, sz, &jerr)) {
    g_printerr("JSON parse error: %s\n", jerr ? jerr->message : "unknown");
    if (jerr) g_error_free(jerr);
    g_object_unref(p);
    return;
  }
  JsonNode* root = json_parser_get_root(p);
  if (!JSON_NODE_HOLDS_OBJECT(root)) { g_object_unref(p); return; }
  JsonObject* obj = json_node_get_object(root);

  const char* type_str = json_member_is_string(obj, "type")
                         ? json_object_get_string_member(obj, "type")
                         : "";

  if (g_strcmp0(type_str, "connected") == 0) {
    // ignore
  } else if (g_strcmp0(type_str, "joined") == 0) {
    // ignore
  } else if (g_strcmp0(type_str, "receiver-joined") == 0) {
    if (app->ws_ready && !app->offer_sent) create_and_send_offer(app);
  } else if (g_strcmp0(type_str, "answer") == 0) {
    handle_answer(app, obj);
  } else if (g_strcmp0(type_str, "ice-candidate") == 0 || g_strcmp0(type_str, "ice") == 0) {
    handle_ice_from_peer(app, obj);
  } else if (g_strcmp0(type_str, "error") == 0) {
    const char* msg = json_member_is_string(obj, "message")
                      ? json_object_get_string_member(obj, "message")
                      : "(no message)";
    g_printerr("[server error] %s\n", msg);
  }

  g_object_unref(p);
}

static void on_ws_closed(SoupWebsocketConnection*, gpointer user_data) {
  App* app = (App*)user_data;
  app->ws_ready = false;
  g_printerr("WebSocket closed.\n");
}

static std::string build_pipeline_str(App* app) {
  std::ostringstream ss;
  
  // Start with v4l2src and format conversion
  ss << "v4l2src device=" << app->device
     << " ! video/x-raw,width=" << app->width << ",height=" << app->height
     << ",framerate=" << app->fps << "/1"
     << " ! videoconvert ! videoscale ! video/x-raw,format=I420";
  
  // Add queue for buffering
  ss << " ! queue max-size-buffers=10 max-size-time=0 max-size-bytes=0";

  if (app->codec == Codec::H264) {
    // Try software encoder first with more conservative settings
    ss << " ! x264enc tune=zerolatency speed-preset=ultrafast"
       << " bitrate=" << app->bitrate_kbps
       << " key-int-max=" << (app->fps * 2) // Keyframe every 2 seconds
       << " ! video/x-h264,profile=baseline"
       << " ! h264parse config-interval=-1"
       << " ! rtph264pay pt=96 config-interval=1 mtu=1200"
       << " ! application/x-rtp,media=video,encoding-name=H264,payload=96";
  } else { // H265
    // Fallback to software encoder for H265 as well
    ss << " ! x265enc tune=zerolatency speed-preset=ultrafast"
       << " bitrate=" << app->bitrate_kbps
       << " key-int-max=" << (app->fps * 2)
       << " ! video/x-h265,profile=main"
       << " ! h265parse config-interval=-1"
       << " ! rtph265pay pt=96 config-interval=1 mtu=1200"
       << " ! application/x-rtp,media=video,encoding-name=H265,payload=96";
  }

  ss << " ! webrtcbin name=sendrecv bundle-policy=max-bundle stun-server=stun://stun.l.google.com:19302";
  return ss.str();
}

// Alternative pipeline using OMX with more conservative settings
static std::string build_omx_pipeline_str(App* app) {
  std::ostringstream ss;
  
  // Start with v4l2src with explicit format
  ss << "v4l2src device=" << app->device
     << " ! video/x-raw,format=NV12,width=" << app->width << ",height=" << app->height
     << ",framerate=" << app->fps << "/1";
  
  // Add conversion and queue
  ss << " ! queue max-size-buffers=10";

  if (app->codec == Codec::H264) {
    // OMX H.264 with baseline profile and conservative settings
    ss << " ! omxh264enc target-bitrate=" << app->bitrate_kbps
       << " control-rate=1"
       << " ! video/x-h264,profile=baseline,level=(string)3.1"
       << " ! h264parse config-interval=-1"
       << " ! rtph264pay pt=96 config-interval=1 mtu=1200"
       << " ! application/x-rtp,media=video,encoding-name=H264,packetization-mode=1,payload=96";
  } else { // H265
    ss << " ! omxh265enc target-bitrate=" << app->bitrate_kbps
       << " control-rate=1"
       << " ! video/x-h265,profile=main"
       << " ! h265parse config-interval=-1"
       << " ! rtph265pay pt=96 config-interval=1 mtu=1200"
       << " ! application/x-rtp,media=video,encoding-name=H265,payload=96";
  }

  ss << " ! webrtcbin name=sendrecv bundle-policy=max-bundle stun-server=stun://stun.l.google.com:19302";
  return ss.str();
}

static gboolean bus_cb(GstBus*, GstMessage* msg, gpointer user_data) {
  App* app = (App*)user_data;
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* e = nullptr; gchar* dbg = nullptr;
      gst_message_parse_error(msg, &e, &dbg);
      g_printerr("ERROR: %s (%s)\n", e->message, dbg ? dbg : "");
      g_clear_error(&e); g_free(dbg);
      g_main_loop_quit(app->loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError* e = nullptr; gchar* dbg = nullptr;
      gst_message_parse_warning(msg, &e, &dbg);
      g_print("WARNING: %s (%s)\n", e->message, dbg ? dbg : "");
      g_clear_error(&e); g_free(dbg);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(app->pipeline)) {
        GstState o,n,p; gst_message_parse_state_changed(msg,&o,&n,&p);
        g_print("Pipeline state: %s\n", gst_element_state_get_name(n));
      }
      break;
    }
    default: break;
  }
  return TRUE;
}

static void on_ws_open(GObject* src, GAsyncResult* res, gpointer user_data) {
  App* app = (App*)user_data;
  GError* err = nullptr;
  app->ws_conn = soup_session_websocket_connect_finish(SOUP_SESSION(src), res, &err);
  if (!app->ws_conn) {
    g_printerr("WebSocket connect failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    g_main_loop_quit(app->loop);
    return;
  }

  g_print("WebSocket connected successfully\n");
  g_signal_connect(app->ws_conn, "message", G_CALLBACK(on_ws_message), app);
  g_signal_connect(app->ws_conn, "closed",  G_CALLBACK(on_ws_closed),  app);

  app->ws_ready = true;

  // Announce: join as sender
  send_join(app);

  // Start pipeline (if not already)
  gst_element_set_state(app->pipeline, GST_STATE_PLAYING);

  // Create an offer proactively
  create_and_send_offer(app);
}

static void parse_args(App* app, int argc, char** argv) {
  for (int i=1; i<argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](int& i)->const char*{ if (i+1<argc) return argv[++i]; return ""; };
    if      (arg.rfind("--ws=",0)==0)       app->ws_url = arg.substr(5);
    else if (arg=="--ws")                   app->ws_url = next(i);
    else if (arg.rfind("--room=",0)==0)     app->room   = arg.substr(7);
    else if (arg=="--room")                 app->room   = next(i);
    else if (arg.rfind("--device=",0)==0)   app->device = arg.substr(9);
    else if (arg=="--device")               app->device = next(i);
    else if (arg.rfind("--width=",0)==0)    app->width  = std::stoi(arg.substr(8));
    else if (arg=="--width")                app->width  = std::stoi(next(i));
    else if (arg.rfind("--height=",0)==0)   app->height = std::stoi(arg.substr(9));
    else if (arg=="--height")               app->height = std::stoi(next(i));
    else if (arg.rfind("--fps=",0)==0)      app->fps    = std::stoi(arg.substr(6));
    else if (arg=="--fps")                  app->fps    = std::stoi(next(i));
    else if (arg.rfind("--bitrate=",0)==0)  app->bitrate_kbps = std::stoi(arg.substr(10));
    else if (arg=="--bitrate")              app->bitrate_kbps = std::stoi(next(i));
    else if (arg.rfind("--codec=",0)==0) {
      auto v = arg.substr(8);
      if (v=="h264") app->codec = Codec::H264;
      else if (v=="h265" || v=="hevc") app->codec = Codec::H265;
      else g_printerr("Unknown codec '%s', defaulting to h264\n", v.c_str());
    }
    else if (arg=="--codec") {
      std::string v = next(i);
      if (v=="h264") app->codec = Codec::H264;
      else if (v=="h265" || v=="hevc") app->codec = Codec::H265;
      else g_printerr("Unknown codec '%s', defaulting to h264\n", v.c_str());
    }
  }
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  App app;
  parse_args(&app, argc, argv);

  app.loop = g_main_loop_new(NULL, FALSE);

  // Try software encoder first (more reliable)
  std::string pipeline_str = build_pipeline_str(&app);
  g_print("Trying software encoder pipeline: %s\n", pipeline_str.c_str());
  
  GError* perr = nullptr;
  app.pipeline = gst_parse_launch(pipeline_str.c_str(), &perr);
  
  // If software encoder fails, try OMX
  if (!app.pipeline || perr) {
    if (perr) {
      g_printerr("Software encoder failed: %s\n", perr->message);
      g_error_free(perr);
      perr = nullptr;
    }
    if (app.pipeline) {
      gst_object_unref(app.pipeline);
      app.pipeline = nullptr;
    }
    
    g_print("Falling back to OMX encoder...\n");
    pipeline_str = build_omx_pipeline_str(&app);
    g_print("OMX Pipeline: %s\n", pipeline_str.c_str());
    app.pipeline = gst_parse_launch(pipeline_str.c_str(), &perr);
  }
  
  if (!app.pipeline) {
    g_printerr("Failed to create pipeline: %s\n", perr ? perr->message : "unknown");
    if (perr) g_error_free(perr);
    return -1;
  }
  
  app.webrtc = gst_bin_get_by_name(GST_BIN(app.pipeline), "sendrecv");
  if (!app.webrtc) {
    g_printerr("Failed to get webrtcbin element\n");
    return -1;
  }

  // Hook WebRTC signals
  g_signal_connect(app.webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), &app);
  g_signal_connect(app.webrtc, "on-ice-candidate",      G_CALLBACK(on_ice_candidate),      &app);

  // Bus logging
  GstBus* bus = gst_element_get_bus(app.pipeline);
  gst_bus_add_watch(bus, bus_cb, &app);
  gst_object_unref(bus);

  // Start to PAUSED; go PLAYING when WS is up
  GstStateChangeReturn ret = gst_element_set_state(app.pipeline, GST_STATE_PAUSED);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to set pipeline to PAUSED state\n");
    return -1;
  }

  // Connect WebSocket to signaling server
  app.soup = soup_session_new();
  app.ws_msg = soup_message_new(SOUP_METHOD_GET, app.ws_url.c_str());
  g_print("Connecting to signaling server: %s\n", app.ws_url.c_str());
  soup_session_websocket_connect_async(
      app.soup, app.ws_msg,
      NULL, // origin
      NULL, // protocols
      NULL, // cancellable
      on_ws_open, &app);

  g_main_loop_run(app.loop);

  // Cleanup
  if (app.ws_conn) soup_websocket_connection_close(app.ws_conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  if (app.pipeline) { gst_element_set_state(app.pipeline, GST_STATE_NULL); gst_object_unref(app.pipeline); }
  if (app.webrtc) gst_object_unref(app.webrtc);
  if (app.ws_msg) g_object_unref(app.ws_msg);
  if (app.soup) g_object_unref(app.soup);
  if (app.loop) g_main_loop_unref(app.loop);
  return 0;
}