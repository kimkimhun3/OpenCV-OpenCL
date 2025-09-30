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

# OpenCV と OpenCL を用いた MPSoC 向けプロジェクト

## 概要
このプロジェクトは、Xilinx MPSoC プラットフォーム上で **OpenCV** による画像処理と **OpenCL** によるハードウェアアクセラレーションを組み合わせ、FPGA を活用したビデオ処理を実現します。  
高解像度ビデオストリームにおいてリアルタイム性能を重視し、組み込み機器上で効率的なコンピュータビジョンを可能にします。

## 特徴
- FPGA アクセラレーションによるリアルタイム映像処理  
- OpenCV を利用した柔軟なフレーム操作  
- OpenCL カーネルによる計算負荷の高い処理のオフロード  
- **2K (1920x1080)** および **4K (3840x2160)** 解像度をサポート  
- **30 FPS および 60 FPS** 動作を想定  

## 最適化ポイント
- 低遅延処理パイプライン  
- CPU と FPGA 間のゼロコピー転送  
- MPSoC のヘテロジニアスアーキテクチャ (ARM + FPGA) を効率的に活用  

## 使用例
- NV12 フォーマット映像の **Y (輝度)** チャンネルに対してヒストグラム平坦化（`equalizeHist`）を適用  
- 色情報を維持したままコントラストを改善  
- 処理後のフレームをストリーミングし、表示や追加のコンピュータビジョンタスクに利用  

## 必要環境
- Xilinx MPSoC ボード (例: ZCU106)  
- GStreamer と OpenCV を含む PetaLinux 環境  
- FPGA 開発用 Vitis / Vivado ツールチェーン  
- OpenCL ランタイム (XRT)  
- NEXT PLAN 

---
