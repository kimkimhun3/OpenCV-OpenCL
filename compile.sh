# Makefile for OpenCV + GStreamer project

# Compiler
CXX = g++

# Compiler flags
CXXFLAGS = -std=c++11 -Wall -O2

# OpenCV flags
OPENCV_CFLAGS = `pkg-config --cflags opencv4`
OPENCV_LIBS = `pkg-config --libs opencv4`

# GStreamer flags
GSTREAMER_CFLAGS = `pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-plugins-base-1.0`
GSTREAMER_LIBS = `pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-plugins-base-1.0`

# Combined flags
ALL_CFLAGS = $(CXXFLAGS) $(OPENCV_CFLAGS) $(GSTREAMER_CFLAGS)
ALL_LIBS = $(OPENCV_LIBS) $(GSTREAMER_LIBS)

# Source files
SOURCES = OpenCVequalHist.cpp
TARGET = Hui

# Default target
all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(ALL_CFLAGS) -o $(TARGET) $(SOURCES) $(ALL_LIBS)

# Clean target
clean:
	rm -f $(TARGET)

# Debug version
debug: CXXFLAGS += -g -DDEBUG
debug: $(TARGET)

# Check dependencies
check-deps:
	@echo "Checking OpenCV..."
	@pkg-config --exists opencv4 && echo "OpenCV 4: OK" || echo "OpenCV 4: MISSING"
	@echo "Checking GStreamer..."
	@pkg-config --exists gstreamer-1.0 && echo "GStreamer: OK" || echo "GStreamer: MISSING"
	@pkg-config --exists gstreamer-app-1.0 && echo "GStreamer App: OK" || echo "GStreamer App: MISSING"
	@pkg-config --exists gstreamer-video-1.0 && echo "GStreamer Video: OK" || echo "GStreamer Video: MISSING"

# Show flags (for debugging)
show-flags:
	@echo "CFLAGS: $(ALL_CFLAGS)"
	@echo "LIBS: $(ALL_LIBS)"

.PHONY: all clean debug check-deps show-flags