# OpenCV and OpenCL in MPSoC

## Overview
This project demonstrates FPGA-accelerated video processing on a Xilinx MPSoC platform by integrating **OpenCV** for image manipulation and **OpenCL** for hardware acceleration.  
The focus is on real-time performance for high-resolution video streams, enabling efficient computer vision applications on embedded hardware.

## Features
- Real-time video processing with FPGA acceleration  
- OpenCV integration for flexible image/frame manipulation  
- OpenCL kernels for offloading compute-intensive tasks  
- Support for **2K (1920x1080)** and **4K (3840x2160)** resolutions  
- Designed for **30 FPS and 60 FPS** processing  

## Key Optimizations
- Low latency processing pipeline  
- Zero-copy memory transfer between CPU and FPGA  
- Efficient use of MPSoC heterogeneous architecture (ARM + FPGA)  

## Use Case Example
- Perform histogram equalization (`equalizeHist`) on the **Y (luma)** channel of NV12 video  
- Enhance image contrast while preserving color fidelity  
- Stream processed frames for display or further computer vision tasks  

## Requirements
- Xilinx MPSoC board (e.g., ZCU106)  
- PetaLinux environment with GStreamer and OpenCV  
- Vitis/Vivado toolchain for FPGA development  
- OpenCL runtime (XRT)  

---
