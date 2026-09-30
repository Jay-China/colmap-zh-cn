// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//     * Neither the name of ETH Zurich and UNC Chapel Hill nor the names of
//       its contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "colmap/ui/automatic_reconstruction_widget.h"

#include "colmap/estimators/bundle_adjustment.h"
#include "colmap/ui/main_window.h"

namespace colmap {

AutomaticReconstructionWidget::AutomaticReconstructionWidget(
    MainWindow* main_window)
    : OptionsWidget(main_window),
      main_window_(main_window),
      thread_control_widget_(new ThreadControlWidget(this)) {
  setWindowFlags(Qt::Dialog);
  setWindowModality(Qt::ApplicationModal);
  setWindowTitle("自动重建");

  AddOptionDirPath(&options_.workspace_path, "工作区文件夹");
  AddSpacer();
  AddOptionDirPath(&options_.image_path, "图像文件夹");
  AddSpacer();
  AddOptionDirPath(&options_.mask_path, "掩膜文件夹");
  AddSpacer();
  AddOptionFilePath(&options_.vocab_tree_path, "词汇树<br>（可选）");

  AddSpacer();

  QLabel* data_type_label = new QLabel(tr("数据类型"), this);
  data_type_label->setFont(font());
  data_type_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  grid_layout_->addWidget(data_type_label, grid_layout_->rowCount(), 0);

  data_type_cb_ = new QComboBox(this);
  data_type_cb_->addItem("单张图像");
  data_type_cb_->addItem("视频帧");
  data_type_cb_->addItem("网络图像");
  grid_layout_->addWidget(data_type_cb_, grid_layout_->rowCount() - 1, 1);

  QLabel* quality_label = new QLabel(tr("质量"), this);
  quality_label->setFont(font());
  quality_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  grid_layout_->addWidget(quality_label, grid_layout_->rowCount(), 0);

  quality_cb_ = new QComboBox(this);
  quality_cb_->addItem("低");
  quality_cb_->addItem("中");
  quality_cb_->addItem("高");
  quality_cb_->addItem("极高");
  quality_cb_->setCurrentIndex(2);
  grid_layout_->addWidget(quality_cb_, grid_layout_->rowCount() - 1, 1);

  AddSpacer();

  AddOptionBool(&options_.single_camera, "共享内参");
  AddOptionBool(&options_.single_camera_per_folder,
                "每个子文件夹共享内参");
  AddOptionBool(&options_.sparse, "稀疏模型");
  AddOptionBool(&options_.dense, "稠密模型");

  QLabel* mesher_label = new QLabel(tr("网格生成器"), this);
  mesher_label->setFont(font());
  mesher_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  grid_layout_->addWidget(mesher_label, grid_layout_->rowCount(), 0);

  mesher_cb_ = new QComboBox(this);
  mesher_cb_->addItem("泊松 (Poisson)");
  mesher_cb_->addItem("德劳内 (Delaunay)");
  mesher_cb_->setCurrentIndex(0);
  grid_layout_->addWidget(mesher_cb_, grid_layout_->rowCount() - 1, 1);

  AddSpacer();

  AddOptionInt(&options_.num_threads, "num_threads", -1);
  AddOptionInt(&options_.random_seed, "random_seed", -1);
  AddOptionBool(&options_.use_gpu, "GPU");
  AddOptionText(&options_.gpu_index, "gpu_index");

#ifdef CASPAR_ENABLED
  AddSpacer();
  AddSection("光束法平差后端");

  ba_backend_cb_ = new QComboBox(this);
  ba_backend_cb_->addItem("CERES");
  ba_backend_cb_->addItem("CASPAR");
  ba_backend_cb_->setCurrentIndex(static_cast<int>(options_.ba_backend));
  AddWidgetRow("Backend", ba_backend_cb_);
#endif

  AddSpacer();

  QPushButton* run_button = new QPushButton(tr("运行"), this);
  grid_layout_->addWidget(run_button, grid_layout_->rowCount(), 1);
  connect(run_button,
          &QPushButton::released,
          this,
          &AutomaticReconstructionWidget::Run);

  render_result_ = new QAction(this);
  connect(render_result_,
          &QAction::triggered,
          this,
          &AutomaticReconstructionWidget::RenderResult,
          Qt::QueuedConnection);
}

void AutomaticReconstructionWidget::Run() {
  WriteOptions();

  if (!ExistsDir(options_.workspace_path)) {
    QMessageBox::critical(this, "", tr("无效的工作区文件夹"));
    return;
  }

  if (!ExistsDir(options_.image_path)) {
    QMessageBox::critical(this, "", tr("无效的图像文件夹"));
    return;
  }

  switch (data_type_cb_->currentIndex()) {
    case 0:
      options_.data_type =
          AutomaticReconstructionController::DataType::INDIVIDUAL;
      break;
    case 1:
      options_.data_type = AutomaticReconstructionController::DataType::VIDEO;
      break;
    case 2:
      options_.data_type =
          AutomaticReconstructionController::DataType::INTERNET;
      break;
    default:
      options_.data_type =
          AutomaticReconstructionController::DataType::INDIVIDUAL;
      break;
  }

  switch (quality_cb_->currentIndex()) {
    case 0:
      options_.quality = AutomaticReconstructionController::Quality::LOW;
      break;
    case 1:
      options_.quality = AutomaticReconstructionController::Quality::MEDIUM;
      break;
    case 2:
      options_.quality = AutomaticReconstructionController::Quality::HIGH;
      break;
    case 3:
      options_.quality = AutomaticReconstructionController::Quality::EXTREME;
      break;
    default:
      options_.quality = AutomaticReconstructionController::Quality::HIGH;
      break;
  }

  switch (mesher_cb_->currentIndex()) {
    case 0:
      options_.mesher = AutomaticReconstructionController::Mesher::POISSON;
      break;
    case 1:
      options_.mesher = AutomaticReconstructionController::Mesher::DELAUNAY;
      break;
    default:
      options_.mesher = AutomaticReconstructionController::Mesher::POISSON;
      break;
  }

#ifdef CASPAR_ENABLED
  options_.ba_backend =
      static_cast<BundleAdjustmentBackend>(ba_backend_cb_->currentIndex());
#endif

  main_window_->reconstruction_manager_->Clear();
  main_window_->reconstruction_manager_widget_->Update();
  main_window_->RenderClear();
  main_window_->RenderNow();

  auto controller = std::make_unique<AutomaticReconstructionController>(
      options_, main_window_->reconstruction_manager_);
  controller->AddCallback(Thread::FINISHED_CALLBACK,
                          [this]() { render_result_->trigger(); });
  controller->Setup();
  thread_control_widget_->StartThread(
      "Reconstructing...", true, std::move(controller));
}

void AutomaticReconstructionWidget::RenderResult() {
  if (main_window_->reconstruction_manager_->Size() > 0) {
    main_window_->reconstruction_manager_widget_->Update();
    main_window_->RenderClear();
    main_window_->RenderNow();
  }

  if (options_.sparse) {
    QMessageBox::information(
        this,
        "",
        tr("已导入重建的稀疏模型用于可视化。模型同时已导出到工作区的 <i>sparse</i> 子文件夹。"));
  }

  if (options_.dense) {
    const auto dense_path = options_.workspace_path / "dense" / "0";
    std::filesystem::path meshing_path;
    if (options_.mesher == AutomaticReconstructionController::Mesher::POISSON) {
      meshing_path = dense_path / "meshed-poisson.ply";
    } else {
      meshing_path = dense_path / "meshed-delaunay.ply";
    }

    if (ExistsFile(meshing_path)) {
      try {
        main_window_->model_viewer_widget_->surface_mesh =
            ReadPlyMesh(meshing_path);
        main_window_->RenderNow();
      } catch (const std::exception& e) {
        LOG(ERROR) << "读取表面网格失败：" << e.what();
      }
    }

    QMessageBox::information(
        this,
        "",
        tr("若要查看重建的稠密点云或表面网格，请通过<i>文件 > 导入…</i>打开工作区中的 <i>dense</i> 子文件夹。"));
  }
}

}  // namespace colmap
