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

#include "colmap/ui/main_window.h"

#include "colmap/controllers/global_pipeline.h"
#include "colmap/controllers/hierarchical_pipeline.h"
#include "colmap/scene/reconstruction_io.h"
#include "colmap/sensor/bitmap.h"
#include "colmap/ui/qt_utils.h"
#include "colmap/ui/render_options.h"
#include "colmap/util/logging.h"
#include "colmap/util/ply.h"
#include "colmap/util/version.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QStandardPaths>
#include <clocale>

#include <QtSvg/QSvgRenderer>

static void InitUiResources() { Q_INIT_RESOURCE(resources); }

namespace colmap {
namespace {

// Keys used with QSettings to persist last-used directories for different
// contexts.
constexpr char kLastGlobalDir[] =
    "last_dir/global";  // Fallback if no context-specific path exists
constexpr char kLastDirProject[] =
    "last_dir/project";  // Last location of open/save project dialogs
constexpr char kLastImportExport[] =
    "last_dir/import_export";  // Last location used in import/export dialogs
constexpr char kLastGrabImage[] =
    "last_dir/grab_image";  // Last location for "grab image" operations

// Get proper QSettings
QSettings GetQSettings() { return QSettings("Colmap", "ColmapUI"); }

QIcon CreateApplicationIcon() {
  const QString logo_path = QStringLiteral(":/media/colmap-logo.svg");
#ifdef Q_OS_MACOS
  // Match the macOS app icon grid, where the artwork occupies roughly 80% of
  // the full canvas instead of extending all the way to its edges.
  constexpr int kCanvasSize = 1024;
  constexpr int kArtworkInset = 100;
  QPixmap icon_pixmap(kCanvasSize, kCanvasSize);
  icon_pixmap.fill(Qt::transparent);

  QSvgRenderer logo_renderer(logo_path);
  QPainter logo_painter(&icon_pixmap);
  logo_renderer.render(&logo_painter,
                       QRectF(kArtworkInset,
                              kArtworkInset,
                              kCanvasSize - 2 * kArtworkInset,
                              kCanvasSize - 2 * kArtworkInset));
  logo_painter.end();
  return QIcon(icon_pixmap);
#else
  return QIcon(logo_path);
#endif
}

// Default fallback: Documents (or home).
QString DefaultBaseDir() {
  QString d =
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (d.isEmpty()) d = QDir::homePath();
  return d;
}

// Load last dir for a given key, falling back to a global last dir,
// and finally to a sensible default.
QString GetLastOpen(const QString& key) {
  QSettings s = GetQSettings();
  s.beginGroup("paths");
  QString per = s.value(key, "").toString();
  if (!per.isEmpty()) {
    s.endGroup();
    return per;
  }
  QString global = s.value(kLastGlobalDir, "").toString();
  s.endGroup();
  if (!global.isEmpty()) return global;
  return DefaultBaseDir();
}

// Save the directory part of a path (or the dir itself) under key,
// and also refresh a global "last dir" to improve first-time UX elsewhere.
void SetLastOpen(const QString& key, const QString& pathOrDir) {
  QString dir = pathOrDir;
  QFileInfo fi(pathOrDir);
  if (!fi.exists() || fi.isFile()) dir = fi.absolutePath();
  if (dir.isEmpty()) dir = DefaultBaseDir();
  QSettings s = GetQSettings();
  s.beginGroup("paths");
  s.setValue(key, dir);
  s.setValue(kLastGlobalDir, dir);
  s.endGroup();
}

std::string GetLogTarget() {
  if (FLAGS_logtostderr) {
    return "stderr";
  }

#if COLMAP_GLOG_HAS_STDOUT_SUPPORT
  if (FLAGS_logtostdout) {
    return "stdout";
  }
#endif

  if (FLAGS_alsologtostderr) {
    return "stderr_and_file";
  }

  return "file";
}

void ApplyLogOptions(const std::string& log_target,
                     int verbosity,
                     int min_severity,
                     bool color) {
  FLAGS_v = verbosity;
  FLAGS_minloglevel = min_severity;

#if COLMAP_GLOG_HAS_COLOR_SUPPORT
  FLAGS_colorlogtostderr = color;
#endif

  FLAGS_logtostderr = false;
#if COLMAP_GLOG_HAS_STDOUT_SUPPORT
  FLAGS_logtostdout = false;
#endif
  FLAGS_alsologtostderr = false;

  if (log_target == "stderr") {
    FLAGS_logtostderr = true;
  } else if (log_target == "stdout") {
#if COLMAP_GLOG_HAS_STDOUT_SUPPORT
    FLAGS_logtostdout = true;
#else
    LOG(WARNING) << "log_target=stdout 需要 glog >= 0.6，回退到 stderr。";
    FLAGS_logtostderr = true;
#endif
  } else if (log_target == "file") {
    // default file logging
  } else if (log_target == "stderr_and_file") {
    FLAGS_alsologtostderr = true;
  } else {
    LOG(ERROR) << "无效的 log_target：" << log_target
               << "。回退到 stderr_and_file。";
    FLAGS_alsologtostderr = true;
  }

#if COLMAP_GLOG_HAS_STDOUT_SUPPORT
  FLAGS_colorlogtostdout = FLAGS_colorlogtostderr;
#endif
}

}  // anonymous namespace

MainWindow::MainWindow(OptionManager options)
    : options_(std::move(options)),
      reconstruction_manager_(std::make_shared<ReconstructionManager>()),
      thread_control_widget_(new ThreadControlWidget(this)),
      window_closed_(false) {
  InitUiResources();

  const QIcon app_icon = CreateApplicationIcon();
  setWindowIcon(app_icon);
  QApplication::setWindowIcon(app_icon);

  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  std::setlocale(LC_NUMERIC, "C");

  resize(1024, 600);
  UpdateWindowTitle();

  setAcceptDrops(true);

  CreateWidgets();
  CreateActions();
  CreateMenus();
  CreateToolbar();
  CreateStatusbar();
  // The mapper controller (and its database cache) is created lazily when a
  // reconstruction is actually started, so opening the GUI does not read the
  // database. Just initialize the control states here.
  UpdateMapperControls();

  ShowLog();

  options_.AddAllOptions();
}

void MainWindow::ImportReconstruction(
    const std::filesystem::path& import_path) {
  if (import_path.empty()) {
    return;
  }

  SetLastOpen(kLastImportExport, QString::fromStdString(import_path.string()));

  const std::filesystem::path project_path = import_path / "project.ini";

  const std::filesystem::path cameras_bin_path = import_path / "cameras.bin";
  const std::filesystem::path images_bin_path = import_path / "images.bin";
  const std::filesystem::path points3D_bin_path = import_path / "points3D.bin";
  const std::filesystem::path cameras_txt_path = import_path / "cameras.txt";
  const std::filesystem::path images_txt_path = import_path / "images.txt";
  const std::filesystem::path points3D_txt_path = import_path / "points3D.txt";

  const bool is_valid_reconstruction_dir =
      (ExistsFile(cameras_bin_path) && ExistsFile(images_bin_path) &&
       ExistsFile(points3D_bin_path)) ||
      (ExistsFile(cameras_txt_path) && ExistsFile(images_txt_path) &&
       ExistsFile(points3D_txt_path));
  if (!is_valid_reconstruction_dir) {
    QMessageBox::critical(this,
                          "",
                          tr("所选目录中不存在 cameras、images 和 points3D 文件。"));
    return;
  }

  if (!ReconstructionOverwrite()) {
    return;
  }

  bool edit_project = false;
  if (ExistsFile(project_path)) {
    options_.ReRead(project_path);
  } else {
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "",
        tr("目录中不包含 <i>project.ini</i>。若要继续该重建，您需要指定有效的数据库和图像路径。是否现在选择路径（或点击<i>否</i>仅查看重建结果）？"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
      edit_project = true;
    }
  }

  thread_control_widget_->StartFunction(
      "Importing...", [this, import_path, edit_project]() {
        const size_t idx = reconstruction_manager_->Read(import_path);
        reconstruction_manager_widget_->Update();
        reconstruction_manager_widget_->SelectReconstruction(idx);
        action_bundle_adjustment_->setEnabled(true);
        action_render_now_->trigger();
        if (edit_project) {
          action_project_edit_->trigger();
        }
      });
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (window_closed_) {
    event->accept();
    return;
  }

  if (project_widget_->IsValid() && options_.project_path->empty()) {
    // Project was created, but not yet saved
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(
        this,
        "",
        tr("您尚未保存项目，是否现在保存？"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
      ProjectSave();
    }
  }

  QMessageBox::StandardButton reply;
  reply = QMessageBox::question(this,
                                "",
                                tr("确定要退出吗？"),
                                QMessageBox::Yes | QMessageBox::No);
  if (reply == QMessageBox::No) {
    event->ignore();
  } else {
    if (mapper_controller_) {
      mapper_controller_->Stop();
      mapper_controller_->Wait();
    }

    log_widget_->close();
    event->accept();

    window_closed_ = true;
  }
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  HandleDragEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event) {
  HandleDragEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event) {
  const QMimeData* mime_data = event->mimeData();
  if (!mime_data->hasUrls()) {
    event->ignore();
    return;
  }

  if (mime_data->urls().size() > 1) {
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "",
        tr("检测到多个路径，将使用第一个路径。是否继续？"),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::No) {
      return;
    }
  }

  const std::string drop_path =
      mime_data->urls().first().toLocalFile().toStdString();

  try {
    if (QFileInfo(QString::fromStdString(drop_path)).isDir()) {
      ImportReconstruction(drop_path);
    } else if (QFileInfo(QString::fromStdString(drop_path))
                   .suffix()
                   .compare("ply", Qt::CaseInsensitive) == 0) {
      ImportFrom(drop_path);
    } else {
      QMessageBox::critical(
          this, "", tr("不支持的文件类型，仅支持 PLY 文件。"));
    }
  } catch (const std::exception& e) {
    QMessageBox::critical(this, "", tr("导入失败：") + e.what());
  }
}

void MainWindow::CreateWidgets() {
  model_viewer_widget_ = new ModelViewerWidget(this, &options_);
  setCentralWidget(model_viewer_widget_);

  project_widget_ = new ProjectWidget(this, &options_);
  project_widget_->SetDatabasePath(*options_.database_path);
  project_widget_->SetImagePath(*options_.image_path);

  feature_extraction_widget_ = new FeatureExtractionWidget(this, &options_);
  feature_matching_widget_ = new FeatureMatchingWidget(this, &options_);
  database_management_widget_ = new DatabaseManagementWidget(this, &options_);
  automatic_reconstruction_widget_ = new AutomaticReconstructionWidget(this);
  reconstruction_options_widget_ = new ReconstructionOptionsWidget(
      this, &options_, &mapper_type_, [this]() { UpdateMapperControls(); });
  bundle_adjustment_widget_ = new BundleAdjustmentWidget(this, &options_);
  dense_reconstruction_widget_ = new DenseReconstructionWidget(this, &options_);
  render_options_widget_ =
      new RenderOptionsWidget(this, &options_, model_viewer_widget_);
  log_widget_ = new LogWidget(this);
  undistortion_widget_ = new UndistortionWidget(this, &options_);
  reconstruction_manager_widget_ =
      new ReconstructionManagerWidget(this, reconstruction_manager_);
  reconstruction_stats_widget_ = new ReconstructionStatsWidget(this);
  match_matrix_widget_ = new MatchMatrixWidget(this, &options_);

  dock_log_widget_ = new QDockWidget("Log", this);
  dock_log_widget_->setWidget(log_widget_);
  addDockWidget(Qt::RightDockWidgetArea, dock_log_widget_);
}

void MainWindow::CreateActions() {
  //////////////////////////////////////////////////////////////////////////////
  // File actions
  //////////////////////////////////////////////////////////////////////////////

  action_project_new_ = new QAction(
      ThemedIcon(":/media/project-new.svg"), tr("新建项目"), this);
  action_project_new_->setShortcuts(QKeySequence::New);
  connect(
      action_project_new_, &QAction::triggered, this, &MainWindow::ProjectNew);

  action_project_open_ = new QAction(
      ThemedIcon(":/media/project-open.svg"), tr("打开项目"), this);
  action_project_open_->setShortcuts(QKeySequence::Open);
  connect(action_project_open_,
          &QAction::triggered,
          this,
          &MainWindow::ProjectOpen);

  action_project_edit_ = new QAction(
      ThemedIcon(":/media/project-edit.svg"), tr("编辑项目"), this);
  connect(action_project_edit_,
          &QAction::triggered,
          this,
          &MainWindow::ProjectEdit);

  action_project_save_ = new QAction(
      ThemedIcon(":/media/project-save.svg"), tr("保存项目"), this);
  action_project_save_->setShortcuts(QKeySequence::Save);
  connect(action_project_save_,
          &QAction::triggered,
          this,
          &MainWindow::ProjectSave);

  action_project_save_as_ =
      new QAction(ThemedIcon(":/media/project-save-as.svg"),
                  tr("项目另存为…"),
                  this);
  action_project_save_as_->setShortcuts(QKeySequence::SaveAs);
  connect(action_project_save_as_,
          &QAction::triggered,
          this,
          &MainWindow::ProjectSaveAs);

  action_import_ =
      new QAction(ThemedIcon(":/media/import.svg"), tr("导入模型"), this);
  action_import_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
  connect(action_import_, &QAction::triggered, this, &MainWindow::Import);
  blocking_actions_.push_back(action_import_);

  action_import_from_ = new QAction(
      ThemedIcon(":/media/import-from.svg"), tr("从…导入"), this);
  connect(action_import_from_,
          &QAction::triggered,
          this,
          QOverload<>::of(&MainWindow::ImportFrom));
  blocking_actions_.push_back(action_import_from_);

  action_export_ =
      new QAction(ThemedIcon(":/media/export.svg"), tr("导出模型"), this);
  action_export_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
  connect(action_export_, &QAction::triggered, this, &MainWindow::Export);
  blocking_actions_.push_back(action_export_);

  action_export_all_ = new QAction(
      ThemedIcon(":/media/export-all.svg"), tr("导出所有模型"), this);
  connect(
      action_export_all_, &QAction::triggered, this, &MainWindow::ExportAll);
  blocking_actions_.push_back(action_export_all_);

  action_export_as_ = new QAction(
      ThemedIcon(":/media/export-as.svg"), tr("导出模型为…"), this);
  connect(action_export_as_, &QAction::triggered, this, &MainWindow::ExportAs);
  blocking_actions_.push_back(action_export_as_);

  action_export_as_text_ = new QAction(ThemedIcon(":/media/export-as-text.svg"),
                                       tr("以文本形式导出模型"),
                                       this);
  connect(action_export_as_text_,
          &QAction::triggered,
          this,
          &MainWindow::ExportAsText);
  blocking_actions_.push_back(action_export_as_text_);

  action_quit_ = new QAction(tr("退出"), this);
  connect(action_quit_, &QAction::triggered, this, &MainWindow::close);

  //////////////////////////////////////////////////////////////////////////////
  // Processing action
  //////////////////////////////////////////////////////////////////////////////

  action_feature_extraction_ =
      new QAction(ThemedIcon(":/media/feature-extraction.svg"),
                  tr("特征提取"),
                  this);
  connect(action_feature_extraction_,
          &QAction::triggered,
          this,
          &MainWindow::FeatureExtraction);
  blocking_actions_.push_back(action_feature_extraction_);

  action_feature_matching_ = new QAction(
      ThemedIcon(":/media/feature-matching.svg"), tr("特征匹配"), this);
  connect(action_feature_matching_,
          &QAction::triggered,
          this,
          &MainWindow::FeatureMatching);
  blocking_actions_.push_back(action_feature_matching_);

  action_database_management_ =
      new QAction(ThemedIcon(":/media/database-management.svg"),
                  tr("数据库管理"),
                  this);
  connect(action_database_management_,
          &QAction::triggered,
          this,
          &MainWindow::DatabaseManagement);
  blocking_actions_.push_back(action_database_management_);

  //////////////////////////////////////////////////////////////////////////////
  // Reconstruction actions
  //////////////////////////////////////////////////////////////////////////////

  action_automatic_reconstruction_ =
      new QAction(ThemedIcon(":/media/automatic-reconstruction.svg"),
                  tr("自动重建"),
                  this);
  connect(action_automatic_reconstruction_,
          &QAction::triggered,
          this,
          &MainWindow::AutomaticReconstruction);

  action_reconstruction_start_ =
      new QAction(ThemedIcon(":/media/reconstruction-start.svg"),
                  tr("开始重建"),
                  this);
  connect(action_reconstruction_start_,
          &QAction::triggered,
          this,
          &MainWindow::ReconstructionStart);
  blocking_actions_.push_back(action_reconstruction_start_);

  action_reconstruction_step_ =
      new QAction(ThemedIcon(":/media/reconstruction-step.svg"),
                  tr("重建下一张图像"),
                  this);
  connect(action_reconstruction_step_,
          &QAction::triggered,
          this,
          &MainWindow::ReconstructionStep);
  blocking_actions_.push_back(action_reconstruction_step_);

  action_reconstruction_pause_ =
      new QAction(ThemedIcon(":/media/reconstruction-pause.svg"),
                  tr("暂停重建"),
                  this);
  connect(action_reconstruction_pause_,
          &QAction::triggered,
          this,
          &MainWindow::ReconstructionPause);
  action_reconstruction_pause_->setEnabled(false);
  blocking_actions_.push_back(action_reconstruction_pause_);

  action_reconstruction_reset_ =
      new QAction(ThemedIcon(":/media/reconstruction-reset.svg"),
                  tr("重置重建"),
                  this);
  connect(action_reconstruction_reset_,
          &QAction::triggered,
          this,
          &MainWindow::ReconstructionOverwrite);

  action_reconstruction_normalize_ =
      new QAction(ThemedIcon(":/media/reconstruction-normalize.svg"),
                  tr("归一化重建"),
                  this);
  connect(action_reconstruction_normalize_,
          &QAction::triggered,
          this,
          &MainWindow::ReconstructionNormalize);
  blocking_actions_.push_back(action_reconstruction_normalize_);

  action_reconstruction_options_ =
      new QAction(ThemedIcon(":/media/reconstruction-options.svg"),
                  tr("重建选项"),
                  this);
  connect(action_reconstruction_options_,
          &QAction::triggered,
          this,
          &MainWindow::ReconstructionOptions);
  blocking_actions_.push_back(action_reconstruction_options_);

  action_bundle_adjustment_ =
      new QAction(ThemedIcon(":/media/bundle-adjustment.svg"),
                  tr("光束法平差"),
                  this);
  connect(action_bundle_adjustment_,
          &QAction::triggered,
          this,
          &MainWindow::BundleAdjustment);
  action_bundle_adjustment_->setEnabled(false);
  blocking_actions_.push_back(action_bundle_adjustment_);

  action_dense_reconstruction_ =
      new QAction(ThemedIcon(":/media/dense-reconstruction.svg"),
                  tr("稠密重建"),
                  this);
  connect(action_dense_reconstruction_,
          &QAction::triggered,
          this,
          &MainWindow::DenseReconstruction);

  //////////////////////////////////////////////////////////////////////////////
  // Render actions
  //////////////////////////////////////////////////////////////////////////////

  action_render_toggle_ = new QAction(
      ThemedIcon(":/media/render-enabled.svg"), tr("禁用渲染"), this);
  connect(action_render_toggle_,
          &QAction::triggered,
          this,
          &MainWindow::RenderToggle);

  action_render_reset_view_ = new QAction(
      ThemedIcon(":/media/render-reset-view.svg"), tr("重置视图"), this);
  connect(action_render_reset_view_,
          &QAction::triggered,
          model_viewer_widget_,
          &ModelViewerWidget::ResetView);

  action_render_options_ = new QAction(
      ThemedIcon(":/media/render-options.svg"), tr("渲染选项"), this);
  connect(action_render_options_,
          &QAction::triggered,
          this,
          &MainWindow::RenderOptions);

  connect(
      reconstruction_manager_widget_,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
      this,
      &MainWindow::SelectReconstructionIdx);

  //////////////////////////////////////////////////////////////////////////////
  // Extras actions
  //////////////////////////////////////////////////////////////////////////////

  action_reconstruction_stats_ =
      new QAction(ThemedIcon(":/media/reconstruction-stats.svg"),
                  tr("显示模型统计"),
                  this);
  connect(action_reconstruction_stats_,
          &QAction::triggered,
          this,
          &MainWindow::ReconstructionStats);

  action_match_matrix_ = new QAction(
      ThemedIcon(":/media/match-matrix.svg"), tr("显示匹配矩阵"), this);
  connect(action_match_matrix_,
          &QAction::triggered,
          this,
          &MainWindow::MatchMatrix);

  action_log_show_ =
      new QAction(ThemedIcon(":/media/log.svg"), tr("显示日志"), this);
  connect(action_log_show_, &QAction::triggered, this, &MainWindow::ShowLog);

  action_grab_image_ =
      new QAction(ThemedIcon(":/media/grab-image.svg"), tr("抓取图像"), this);
  connect(
      action_grab_image_, &QAction::triggered, this, &MainWindow::GrabImage);

  action_grab_movie_ =
      new QAction(ThemedIcon(":/media/grab-movie.svg"), tr("抓取影片"), this);
  connect(action_grab_movie_,
          &QAction::triggered,
          model_viewer_widget_,
          &ModelViewerWidget::GrabMovie);

  action_undistort_ = new QAction(
      ThemedIcon(":/media/undistort.svg"), tr("去畸变"), this);
  connect(action_undistort_,
          &QAction::triggered,
          this,
          &MainWindow::UndistortImages);
  blocking_actions_.push_back(action_undistort_);

  action_extract_colors_ = new QAction(tr("提取颜色"), this);
  connect(action_extract_colors_,
          &QAction::triggered,
          this,
          &MainWindow::ExtractColors);

  action_set_options_ = new QAction(tr("设置…的选项"), this);
  connect(
      action_set_options_, &QAction::triggered, this, &MainWindow::SetOptions);

  action_reset_options_ = new QAction(tr("恢复默认选项"), this);
  connect(action_reset_options_,
          &QAction::triggered,
          this,
          &MainWindow::ResetOptions);

  action_set_log_options_ = new QAction(tr("设置日志选项"), this);
  connect(action_set_log_options_,
          &QAction::triggered,
          this,
          &MainWindow::SetLogOptions);

  //////////////////////////////////////////////////////////////////////////////
  // Misc actions
  //////////////////////////////////////////////////////////////////////////////

  action_render_ = new QAction(tr("渲染"), this);
  connect(action_render_,
          &QAction::triggered,
          this,
          &MainWindow::Render,
          Qt::BlockingQueuedConnection);

  action_render_now_ = new QAction(tr("立即渲染"), this);
  render_options_widget_->action_render_now = action_render_now_;
  connect(action_render_now_,
          &QAction::triggered,
          this,
          &MainWindow::RenderNow,
          Qt::BlockingQueuedConnection);

  action_reconstruction_finish_ =
      new QAction(tr("完成重建"), this);
  connect(action_reconstruction_finish_,
          &QAction::triggered,
          this,
          &MainWindow::ReconstructionFinish,
          Qt::BlockingQueuedConnection);

  action_about_ = new QAction(tr("关于"), this);
  connect(action_about_, &QAction::triggered, this, &MainWindow::About);
  action_documentation_ = new QAction(tr("文档"), this);
  connect(action_documentation_,
          &QAction::triggered,
          this,
          &MainWindow::Documentation);
  action_support_ = new QAction(tr("支持"), this);
  connect(action_support_, &QAction::triggered, this, &MainWindow::Support);
  action_license_ = new QAction(tr("许可协议"), this);
  connect(action_license_, &QAction::triggered, this, &MainWindow::License);
}

void MainWindow::CreateMenus() {
  QMenu* file_menu = new QMenu(tr("文件"), this);
  file_menu->addAction(action_project_new_);
  file_menu->addAction(action_project_open_);
  file_menu->addAction(action_project_edit_);
  file_menu->addAction(action_project_save_);
  file_menu->addAction(action_project_save_as_);
  file_menu->addSeparator();
  file_menu->addAction(action_import_);
  file_menu->addAction(action_import_from_);
  file_menu->addSeparator();
  file_menu->addAction(action_export_);
  file_menu->addAction(action_export_all_);
  file_menu->addAction(action_export_as_);
  file_menu->addAction(action_export_as_text_);
  file_menu->addSeparator();
  file_menu->addAction(action_quit_);
  menuBar()->addAction(file_menu->menuAction());

  QMenu* preprocessing_menu = new QMenu(tr("处理"), this);
  preprocessing_menu->addAction(action_feature_extraction_);
  preprocessing_menu->addAction(action_feature_matching_);
  preprocessing_menu->addAction(action_database_management_);
  menuBar()->addAction(preprocessing_menu->menuAction());

  QMenu* reconstruction_menu = new QMenu(tr("重建"), this);
  reconstruction_menu->addAction(action_automatic_reconstruction_);
  reconstruction_menu->addSeparator();
  reconstruction_menu->addAction(action_reconstruction_start_);
  reconstruction_menu->addAction(action_reconstruction_pause_);
  reconstruction_menu->addAction(action_reconstruction_step_);
  reconstruction_menu->addSeparator();
  reconstruction_menu->addAction(action_reconstruction_reset_);
  reconstruction_menu->addAction(action_reconstruction_normalize_);
  reconstruction_menu->addAction(action_reconstruction_options_);
  reconstruction_menu->addSeparator();
  reconstruction_menu->addAction(action_bundle_adjustment_);
  reconstruction_menu->addAction(action_dense_reconstruction_);
  menuBar()->addAction(reconstruction_menu->menuAction());

  QMenu* render_menu = new QMenu(tr("渲染"), this);
  render_menu->addAction(action_render_toggle_);
  render_menu->addAction(action_render_reset_view_);
  render_menu->addAction(action_render_options_);
  menuBar()->addAction(render_menu->menuAction());

  QMenu* extras_menu = new QMenu(tr("附加功能"), this);
  extras_menu->addAction(action_log_show_);
  extras_menu->addAction(action_match_matrix_);
  extras_menu->addAction(action_reconstruction_stats_);
  extras_menu->addSeparator();
  extras_menu->addAction(action_grab_image_);
  extras_menu->addAction(action_grab_movie_);
  extras_menu->addSeparator();
  extras_menu->addAction(action_undistort_);
  extras_menu->addAction(action_extract_colors_);
  extras_menu->addSeparator();
  extras_menu->addAction(action_set_options_);
  extras_menu->addAction(action_reset_options_);
  extras_menu->addAction(action_set_log_options_);
  menuBar()->addAction(extras_menu->menuAction());

  QMenu* help_menu = new QMenu(tr("帮助"), this);
  help_menu->addAction(action_about_);
  help_menu->addAction(action_documentation_);
  help_menu->addAction(action_support_);
  help_menu->addAction(action_license_);
  menuBar()->addAction(help_menu->menuAction());
}

void MainWindow::CreateToolbar() {
  file_toolbar_ = addToolBar(tr("文件"));
  file_toolbar_->addAction(action_project_new_);
  file_toolbar_->addAction(action_project_open_);
  file_toolbar_->addAction(action_project_edit_);
  file_toolbar_->addAction(action_project_save_);
  file_toolbar_->addAction(action_import_);
  file_toolbar_->addAction(action_export_);
  file_toolbar_->setIconSize(QSize(16, 16));

  preprocessing_toolbar_ = addToolBar(tr("处理"));
  preprocessing_toolbar_->addAction(action_feature_extraction_);
  preprocessing_toolbar_->addAction(action_feature_matching_);
  preprocessing_toolbar_->addAction(action_database_management_);
  preprocessing_toolbar_->setIconSize(QSize(16, 16));

  reconstruction_toolbar_ = addToolBar(tr("重建"));
  reconstruction_toolbar_->addAction(action_automatic_reconstruction_);
  reconstruction_toolbar_->addAction(action_reconstruction_start_);
  reconstruction_toolbar_->addAction(action_reconstruction_step_);
  reconstruction_toolbar_->addAction(action_reconstruction_pause_);
  reconstruction_toolbar_->addAction(action_reconstruction_options_);
  reconstruction_toolbar_->addAction(action_bundle_adjustment_);
  reconstruction_toolbar_->addAction(action_dense_reconstruction_);
  reconstruction_toolbar_->setIconSize(QSize(16, 16));

  render_toolbar_ = addToolBar(tr("渲染"));
  render_toolbar_->addAction(action_render_toggle_);
  render_toolbar_->addAction(action_render_reset_view_);
  render_toolbar_->addAction(action_render_options_);
  render_toolbar_->addWidget(reconstruction_manager_widget_);
  render_toolbar_->setIconSize(QSize(16, 16));

  extras_toolbar_ = addToolBar(tr("附加功能"));
  extras_toolbar_->addAction(action_log_show_);
  extras_toolbar_->addAction(action_match_matrix_);
  extras_toolbar_->addAction(action_reconstruction_stats_);
  extras_toolbar_->addAction(action_grab_image_);
  extras_toolbar_->addAction(action_grab_movie_);
  extras_toolbar_->setIconSize(QSize(16, 16));
}

void MainWindow::CreateStatusbar() {
  QFont font;
  font.setPointSize(11);

  statusbar_timer_label_ = new QLabel("时间 00:00:00:00", this);
  statusbar_timer_label_->setFont(font);
  statusbar_timer_label_->setAlignment(Qt::AlignCenter);
  statusBar()->addWidget(statusbar_timer_label_, 1);
  statusbar_timer_ = new QTimer(this);
  connect(statusbar_timer_, &QTimer::timeout, this, &MainWindow::UpdateTimer);
  statusbar_timer_->start(1000);

  model_viewer_widget_->statusbar_status_label =
      new QLabel("0 帧 - 0 图像 - 0 点", this);
  model_viewer_widget_->statusbar_status_label->setFont(font);
  model_viewer_widget_->statusbar_status_label->setAlignment(Qt::AlignCenter);
  statusBar()->addWidget(model_viewer_widget_->statusbar_status_label, 1);
}

void MainWindow::CreateMapperController() {
  StopMapperController();

  options_.mapper->image_path = *options_.image_path;

  // Triggers an immediate render of the current reconstruction, unless the
  // mapper is being stopped.
  const auto render_now = [this]() {
    if (!mapper_controller_->IsStopped()) {
      action_render_now_->trigger();
    }
  };
  // Triggers a rate-limited render of the current reconstruction.
  const auto render = [this]() {
    if (!mapper_controller_->IsStopped()) {
      action_render_->trigger();
    }
  };

  switch (mapper_type_) {
    case MapperType::INCREMENTAL: {
      auto controller = std::make_unique<ControllerThread<IncrementalPipeline>>(
          std::make_shared<IncrementalPipeline>(
              options_.mapper,
              Database::Open(*options_.database_path),
              reconstruction_manager_));
      controller->GetController()->AddCallback(
          IncrementalPipeline::INITIAL_IMAGE_PAIR_REG_CALLBACK, render_now);
      controller->GetController()->AddCallback(
          IncrementalPipeline::NEXT_IMAGE_REG_CALLBACK, render);
      controller->GetController()->AddCallback(
          IncrementalPipeline::LAST_IMAGE_REG_CALLBACK, render_now);
      mapper_controller_ = std::move(controller);
      break;
    }
    case MapperType::GLOBAL: {
      GlobalPipelineOptions global_options = *options_.global_mapper;
      global_options.image_path = *options_.image_path;
      auto controller = std::make_unique<ControllerThread<GlobalPipeline>>(
          std::make_shared<GlobalPipeline>(
              std::move(global_options),
              Database::Open(*options_.database_path),
              reconstruction_manager_));
      // The global mapper only renders after global positioning and each
      // refinement, via this callback.
      controller->GetController()->AddCallback(
          GlobalPipeline::MODEL_UPDATE_CALLBACK, render_now);
      mapper_controller_ = std::move(controller);
      break;
    }
    case MapperType::HIERARCHICAL: {
      HierarchicalPipelineOptions hierarchical_options =
          *options_.hierarchical_mapper;
      hierarchical_options.image_path = *options_.image_path;
      hierarchical_options.incremental_options = *options_.mapper;
      // The hierarchical mapper reconstructs clusters in separate managers and
      // only populates the main reconstruction at the end, so no intermediate
      // render callback is wired; only the finished callback below renders.
      mapper_controller_ =
          std::make_unique<ControllerThread<HierarchicalPipeline>>(
              std::make_shared<HierarchicalPipeline>(
                  hierarchical_options,
                  Database::Open(*options_.database_path),
                  reconstruction_manager_));
      break;
    }
  }

  mapper_controller_->AddCallback(Thread::FINISHED_CALLBACK, [this]() {
    if (!mapper_controller_->IsStopped()) {
      action_render_now_->trigger();
      action_reconstruction_finish_->trigger();
    }
    if (reconstruction_manager_->Size() == 0) {
      action_reconstruction_reset_->trigger();
    }
  });

  UpdateMapperControls();
}

void MainWindow::StopMapperController() {
  if (mapper_controller_) {
    mapper_controller_->Stop();
    mapper_controller_->Wait();
    mapper_controller_.reset();
  }
}

void MainWindow::ResetMapperController() {
  // Tear down any existing controller without building a new one (which would
  // eagerly read the database). A fresh controller is created lazily in
  // ReconstructionStart().
  StopMapperController();
  UpdateMapperControls();
}

void MainWindow::HandleDragEvent(QDropEvent* event) {
  if (event->mimeData()->hasUrls()) {
    event->acceptProposedAction();
  } else {
    event->ignore();
  }
}

void MainWindow::ProjectNew() {
  if (ReconstructionOverwrite()) {
    project_widget_->Reset();
    project_widget_->show();
    project_widget_->raise();
  }
}

bool MainWindow::ProjectOpen() {
  if (!ReconstructionOverwrite()) {
    return false;
  }

  const std::string project_path =
      QFileDialog::getOpenFileName(this,
                                   tr("选择项目文件"),
                                   GetLastOpen(kLastDirProject),
                                   tr("项目文件 (*.ini)"))
          .toUtf8()
          .constData();

  if (project_path.empty()) {
    // Selection cancelled.
    return false;
  }

  if (options_.ReRead(project_path)) {
    *options_.project_path = project_path;
    project_widget_->SetDatabasePath(*options_.database_path);
    project_widget_->SetImagePath(*options_.image_path);
    UpdateWindowTitle();
    SetLastOpen(kLastDirProject, QString::fromStdString(project_path));
    return true;
  }

  ShowInvalidProjectError();

  return false;
}

void MainWindow::ProjectEdit() {
  project_widget_->show();
  project_widget_->raise();
}

void MainWindow::ProjectSave() {
  if (!ExistsFile(*options_.project_path)) {
    std::string project_path =
        QFileDialog::getSaveFileName(this,
                                     tr("选择项目文件"),
                                     GetLastOpen(kLastDirProject),
                                     tr("项目文件 (*.ini)"))
            .toUtf8()
            .constData();
    if (!project_path.empty()) {
      if (!HasFileExtension(project_path, ".ini")) {
        project_path += ".ini";
      }
      *options_.project_path = project_path;
      options_.Write(*options_.project_path);
      SetLastOpen(kLastDirProject, QString::fromStdString(project_path));
    }
  } else {
    // Project path was chosen previously, either here or via command-line.
    options_.Write(*options_.project_path);
    SetLastOpen(kLastDirProject,
                QString::fromStdString(options_.project_path->string()));
  }

  UpdateWindowTitle();
}

void MainWindow::ProjectSaveAs() {
  const std::string new_project_path =
      QFileDialog::getSaveFileName(this,
                                   tr("选择项目文件"),
                                   GetLastOpen(kLastDirProject),
                                   tr("项目文件 (*.ini)"))
          .toUtf8()
          .constData();
  if (new_project_path != "") {
    *options_.project_path = new_project_path;
    options_.Write(*options_.project_path);
    SetLastOpen(kLastDirProject, QString::fromStdString(new_project_path));
  }

  UpdateWindowTitle();
}

void MainWindow::Import() {
  const std::string import_path =
      QFileDialog::getExistingDirectory(this,
                                        tr("选择来源…"),
                                        GetLastOpen(kLastImportExport),
                                        QFileDialog::ShowDirsOnly)
          .toUtf8()
          .constData();

  ImportReconstruction(import_path);
}

void MainWindow::ImportFrom() {
  const std::string import_path =
      QFileDialog::getOpenFileName(this,
                                   tr("选择 PLY 文件…"),
                                   GetLastOpen(kLastImportExport),
                                   tr("PLY 文件 (*.ply)"))
          .toUtf8()
          .constData();

  if (import_path.empty()) {
    // Selection cancelled.
    return;
  }

  SetLastOpen(kLastImportExport, QString::fromStdString(import_path));
  ImportFrom(import_path);
}

void MainWindow::ImportFrom(const std::string& import_path) {
  if (!ExistsFile(import_path)) {
    QMessageBox::critical(this, "", tr("无效的文件"));
    return;
  }

  if (HasPlyMeshFaces(import_path)) {
    thread_control_widget_->StartFunction(
        "正在导入表面网格…", [this, import_path]() {
          try {
            PlyTexturedMesh textured_mesh = ReadPlyMesh(import_path);

            // Clear any previous texture data.
            model_viewer_widget_->surface_texture_data.clear();
            model_viewer_widget_->surface_texture_width = 0;
            model_viewer_widget_->surface_texture_height = 0;

            // Load texture atlas if the mesh has UV coordinates and a
            // texture file reference.
            if (!textured_mesh.face_uvs.empty() &&
                !textured_mesh.texture_file.empty()) {
              const auto ply_dir =
                  std::filesystem::path(import_path).parent_path();
              const auto texture_path = ply_dir / textured_mesh.texture_file;
              Bitmap bitmap;
              if (bitmap.Read(texture_path, /*as_rgb=*/true)) {
                model_viewer_widget_->surface_texture_data =
                    bitmap.RowMajorData();
                model_viewer_widget_->surface_texture_width = bitmap.Width();
                model_viewer_widget_->surface_texture_height = bitmap.Height();
              } else {
                LOG(WARNING) << "读取纹理文件失败：" << texture_path
                             << "。回退到顶点颜色。";
                textured_mesh.face_uvs.clear();
              }
            }

            model_viewer_widget_->surface_mesh = std::move(textured_mesh);
            action_render_now_->trigger();
          } catch (const std::exception& e) {
            LOG(ERROR) << "读取表面网格失败：" << e.what();
          }
        });
  } else {
    thread_control_widget_->StartFunction(
        "正在导入点云…", [this, import_path]() {
          try {
            model_viewer_widget_->point_cloud = ReadPly(import_path);
            action_render_now_->trigger();
          } catch (const std::exception& e) {
            LOG(ERROR) << "读取点云失败：" << e.what();
          }
        });
  }
}

void MainWindow::Export() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }

  const std::filesystem::path export_path =
      QFileDialog::getExistingDirectory(this,
                                        tr("选择目标位置…"),
                                        GetLastOpen(kLastImportExport),
                                        QFileDialog::ShowDirsOnly)
          .toUtf8()
          .constData();

  // Selection cancelled?
  if (export_path.empty()) {
    return;
  }

  SetLastOpen(kLastImportExport, QString::fromStdString(export_path.string()));

  const std::string cameras_name = "cameras.bin";
  const std::string images_name = "images.bin";
  const std::string points3D_name = "points3D.bin";

  const std::filesystem::path project_path = export_path / "project.ini";
  const std::filesystem::path cameras_path = export_path / cameras_name;
  const std::filesystem::path images_path = export_path / images_name;
  const std::filesystem::path points3D_path = export_path / points3D_name;

  if (ExistsFile(cameras_path) || ExistsFile(images_path) ||
      ExistsFile(points3D_path)) {
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "",
        StringPrintf(
            "文件 <i>%s</i>、<i>%s</i> 或 <i>%s</i> 在所选目标位置已存在，是否覆盖它们？",
            cameras_name.c_str(),
            images_name.c_str(),
            points3D_name.c_str())
            .c_str(),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::No) {
      return;
    }
  }

  thread_control_widget_->StartFunction(
      "Exporting...", [this, export_path, project_path]() {
        reconstruction_manager_->Get(SelectedReconstructionIdx())
            ->WriteBinary(export_path);
        options_.Write(project_path);
      });
}

void MainWindow::ExportAll() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }

  const std::filesystem::path export_path =
      QFileDialog::getExistingDirectory(this,
                                        tr("选择目标位置…"),
                                        GetLastOpen(kLastImportExport),
                                        QFileDialog::ShowDirsOnly)
          .toUtf8()
          .constData();

  if (export_path.empty()) {
    // Selection cancelled.
    return;
  }

  SetLastOpen(kLastImportExport, QString::fromStdString(export_path.string()));

  thread_control_widget_->StartFunction("Exporting...", [this, export_path]() {
    reconstruction_manager_->Write(export_path);
    options_.Write(export_path / "project.ini");
  });
}

void MainWindow::ExportAs() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }

  QString filter("NVM (*.nvm)");
  const std::filesystem::path export_path =
      QFileDialog::getSaveFileName(
          this,
          tr("选择目标位置…"),
          GetLastOpen(kLastImportExport),
          "NVM (*.nvm);;Bundler (*.out);;PLY (*.ply);;VRML (*.wrl)",
          &filter)
          .toUtf8()
          .constData();

  if (export_path.empty()) {
    // Selection cancelled.
    return;
  }

  SetLastOpen(kLastImportExport, QString::fromStdString(export_path.string()));

  thread_control_widget_->StartFunction(
      "Exporting...", [this, export_path, filter]() {
        const std::shared_ptr<Reconstruction> reconstruction =
            reconstruction_manager_->Get(SelectedReconstructionIdx());
        if (filter == "NVM (*.nvm)") {
          ExportNVM(*reconstruction, export_path);
        } else if (filter == "Bundler (*.out)") {
          ExportBundler(*reconstruction,
                        export_path,
                        AddFileExtension(export_path, ".list.txt"));
        } else if (filter == "PLY (*.ply)") {
          ExportPLY(*reconstruction, export_path);
        } else if (filter == "VRML (*.wrl)") {
          const auto base_path = export_path.parent_path() / export_path.stem();
          ExportVRML(*reconstruction,
                     AddFileExtension(base_path, ".images.wrl"),
                     AddFileExtension(base_path, ".points3D.wrl"),
                     1,
                     Eigen::Vector3d(1, 0, 0));
        }
      });
}

void MainWindow::ExportAsText() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }

  const std::filesystem::path export_path =
      QFileDialog::getExistingDirectory(this,
                                        tr("选择目标位置…"),
                                        GetLastOpen(kLastImportExport),
                                        QFileDialog::ShowDirsOnly)
          .toUtf8()
          .constData();

  if (export_path.empty()) {
    // Selection cancelled.
    return;
  }

  SetLastOpen(kLastImportExport, QString::fromStdString(export_path.string()));

  const std::string cameras_name = "cameras.txt";
  const std::string images_name = "images.txt";
  const std::string points3D_name = "points3D.txt";

  const std::filesystem::path project_path = export_path / "project.ini";
  const std::filesystem::path cameras_path = export_path / cameras_name;
  const std::filesystem::path images_path = export_path / images_name;
  const std::filesystem::path points3D_path = export_path / points3D_name;

  if (ExistsFile(cameras_path) || ExistsFile(images_path) ||
      ExistsFile(points3D_path)) {
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "",
        StringPrintf(
            "文件 <i>%s</i>、<i>%s</i> 或 <i>%s</i> 在所选目标位置已存在，是否覆盖它们？",
            cameras_name.c_str(),
            images_name.c_str(),
            points3D_name.c_str())
            .c_str(),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::No) {
      return;
    }
  }

  thread_control_widget_->StartFunction(
      "Exporting...", [this, export_path, project_path]() {
        reconstruction_manager_->Get(SelectedReconstructionIdx())
            ->WriteText(export_path);
        options_.Write(project_path);
      });
}

void MainWindow::FeatureExtraction() {
  if (options_.Check()) {
    feature_extraction_widget_->show();
    feature_extraction_widget_->raise();
  } else {
    ShowInvalidProjectError();
  }
}

void MainWindow::FeatureMatching() {
  if (options_.Check()) {
    feature_matching_widget_->show();
    feature_matching_widget_->raise();
  } else {
    ShowInvalidProjectError();
  }
}

void MainWindow::DatabaseManagement() {
  if (options_.Check()) {
    database_management_widget_->show();
    database_management_widget_->raise();
  } else {
    ShowInvalidProjectError();
  }
}

void MainWindow::AutomaticReconstruction() {
  automatic_reconstruction_widget_->show();
  automatic_reconstruction_widget_->raise();
}

void MainWindow::ReconstructionStart() {
  const bool started = mapper_controller_ && mapper_controller_->IsStarted();
  const bool finished = mapper_controller_ && mapper_controller_->IsFinished();

  if (!started && !options_.Check()) {
    ShowInvalidProjectError();
    return;
  }

  if (finished && HasSelectedReconstruction()) {
    QMessageBox::critical(
        this, "", tr("开始前请先重置重建。"));
    return;
  }

  if (started) {
    // Resume existing reconstruction.
    timer_.Resume();
    mapper_controller_->Resume();
  } else {
    // Start new reconstruction.
    CreateMapperController();
    timer_.Restart();
    mapper_controller_->Start();
    action_reconstruction_start_->setText(tr("继续重建"));
  }

  DisableBlockingActions();
  action_reconstruction_pause_->setEnabled(true);
  UpdateMapperControls();
}

void MainWindow::ReconstructionStep() {
  // Stepping is only supported for the incremental and global mappers.
  if (mapper_type_ == MapperType::HIERARCHICAL) {
    return;
  }

  if (mapper_controller_ && mapper_controller_->IsFinished() &&
      HasSelectedReconstruction()) {
    QMessageBox::critical(
        this, "", tr("开始前请先重置重建。"));
    return;
  }

  action_reconstruction_step_->setEnabled(false);
  ReconstructionStart();
  ReconstructionPause();
  action_reconstruction_step_->setEnabled(true);
}

void MainWindow::ReconstructionPause() {
  if (!mapper_controller_) {
    return;
  }
  timer_.Pause();
  mapper_controller_->Pause();
  EnableBlockingActions();
  action_reconstruction_pause_->setEnabled(false);
}

void MainWindow::ReconstructionOptions() {
  reconstruction_options_widget_->show();
  reconstruction_options_widget_->raise();
}

void MainWindow::ReconstructionFinish() {
  timer_.Pause();
  mapper_controller_->Stop();
  EnableBlockingActions();
  action_reconstruction_start_->setEnabled(false);
  action_reconstruction_step_->setEnabled(false);
  action_reconstruction_pause_->setEnabled(false);
}

void MainWindow::ReconstructionReset() {
  ResetMapperController();

  reconstruction_manager_->Clear();
  reconstruction_manager_widget_->Update();

  EnableBlockingActions();
  action_reconstruction_start_->setText(tr("开始重建"));
  action_reconstruction_pause_->setEnabled(false);

  RenderClear();
}

void MainWindow::ReconstructionNormalize() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }
  action_reconstruction_step_->setEnabled(false);
  reconstruction_manager_->Get(SelectedReconstructionIdx())->Normalize();
  action_reconstruction_step_->setEnabled(true);
}

bool MainWindow::ReconstructionOverwrite() {
  if (reconstruction_manager_->Size() == 0) {
    ReconstructionReset();
    return true;
  }

  QMessageBox::StandardButton reply = QMessageBox::question(
      this,
      "",
      tr("确定要覆盖已有的重建结果吗？"),
      QMessageBox::Yes | QMessageBox::No);
  if (reply == QMessageBox::No) {
    return false;
  } else {
    ReconstructionReset();
    return true;
  }
}

void MainWindow::BundleAdjustment() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }

  bundle_adjustment_widget_->Show(
      reconstruction_manager_->Get(SelectedReconstructionIdx()));
}

void MainWindow::DenseReconstruction() {
  if (HasSelectedReconstruction()) {
    dense_reconstruction_widget_->Show(
        reconstruction_manager_->Get(SelectedReconstructionIdx()));
  } else {
    dense_reconstruction_widget_->Show(nullptr);
  }
}

void MainWindow::Render() {
  if (reconstruction_manager_->Size() == 0) {
    return;
  }

  int refresh_rate;
  if (options_.render->adapt_refresh_rate) {
    const auto num_reg_frames =
        reconstruction_manager_->Get(SelectedReconstructionIdx())
            ->NumRegFrames();
    refresh_rate = static_cast<int>(num_reg_frames / 50 + 1);
  } else {
    refresh_rate = options_.render->refresh_rate;
  }

  if (!render_options_widget_->automatic_update ||
      render_options_widget_->counter % refresh_rate != 0) {
    render_options_widget_->counter += 1;
    return;
  }

  render_options_widget_->counter += 1;

  RenderNow();
}

void MainWindow::RenderNow() {
  reconstruction_manager_widget_->Update();
  RenderSelectedReconstruction();
}

void MainWindow::RenderSelectedReconstruction() {
  if (reconstruction_manager_->Size() == 0) {
    if (model_viewer_widget_->surface_mesh.has_value() ||
        model_viewer_widget_->point_cloud.has_value()) {
      model_viewer_widget_->reconstruction = nullptr;
      model_viewer_widget_->ReloadReconstruction();
    } else {
      RenderClear();
    }
    return;
  }

  const size_t reconstruction_idx = SelectedReconstructionIdx();
  model_viewer_widget_->reconstruction =
      reconstruction_manager_->Get(reconstruction_idx);
  model_viewer_widget_->ReloadReconstruction();
}

void MainWindow::RenderClear() {
  reconstruction_manager_widget_->SelectReconstruction(
      ReconstructionManagerWidget::kNewestReconstructionIdx);
  model_viewer_widget_->ClearReconstruction();
  timer_.Reset();
  UpdateTimer();
}

void MainWindow::RenderOptions() {
  render_options_widget_->show();
  render_options_widget_->raise();
}

void MainWindow::SelectReconstructionIdx(const size_t) {
  RenderSelectedReconstruction();
}

size_t MainWindow::SelectedReconstructionIdx() {
  size_t reconstruction_idx =
      reconstruction_manager_widget_->SelectedReconstructionIdx();
  if (reconstruction_idx ==
      ReconstructionManagerWidget::kNewestReconstructionIdx) {
    if (reconstruction_manager_->Size() > 0) {
      reconstruction_idx = reconstruction_manager_->Size() - 1;
    }
  }
  return reconstruction_idx;
}

bool MainWindow::HasSelectedReconstruction() {
  const size_t reconstruction_idx =
      reconstruction_manager_widget_->SelectedReconstructionIdx();
  if (reconstruction_idx ==
      ReconstructionManagerWidget::kNewestReconstructionIdx) {
    if (reconstruction_manager_->Size() == 0) {
      return false;
    }
  }
  return true;
}

bool MainWindow::IsSelectedReconstructionValid() {
  if (!HasSelectedReconstruction()) {
    QMessageBox::critical(this, "", tr("未选择重建结果"));
    return false;
  }
  return true;
}

void MainWindow::GrabImage() {
  QString file_name = QFileDialog::getSaveFileName(this,
                                                   tr("保存图像"),
                                                   GetLastOpen(kLastGrabImage),
                                                   tr("图像 (*.png *.jpg)"));
  if (file_name != "") {
    if (!HasFileExtension(file_name.toUtf8().constData(), ".png") &&
        !HasFileExtension(file_name.toUtf8().constData(), ".jpg")) {
      file_name += ".png";
    }
    QImage image = model_viewer_widget_->GrabImage();
    image.save(file_name);
    SetLastOpen(kLastGrabImage, file_name);
  }
}

void MainWindow::UndistortImages() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }
  undistortion_widget_->Show(
      reconstruction_manager_->Get(SelectedReconstructionIdx()));
}

void MainWindow::ReconstructionStats() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }
  reconstruction_stats_widget_->show();
  reconstruction_stats_widget_->raise();
  reconstruction_stats_widget_->Show(
      *reconstruction_manager_->Get(SelectedReconstructionIdx()));
}

void MainWindow::MatchMatrix() { match_matrix_widget_->Show(); }

void MainWindow::ShowLog() {
  log_widget_->show();
  log_widget_->raise();
  dock_log_widget_->show();
  dock_log_widget_->raise();
}

void MainWindow::ExtractColors() {
  if (!IsSelectedReconstructionValid()) {
    return;
  }

  thread_control_widget_->StartFunction("正在提取颜色…", [this]() {
    reconstruction_manager_->Get(SelectedReconstructionIdx())
        ->ExtractColorsForAllImages(*options_.image_path);
  });
}

void MainWindow::SetOptions() {
  QStringList data_items;
  data_items << "单张图像"
             << "视频帧"
             << "网络图像";
  bool data_ok = false;
  const QString data_item =
      QInputDialog::getItem(this, "", "数据类型：", data_items, 0, false, &data_ok);
  if (!data_ok) {
    return;
  }

  QStringList quality_items;
  quality_items << "低"
                << "中"
                << "高"
                << "极高";
  bool quality_ok = false;
  const QString quality_item = QInputDialog::getItem(
      this, "", "质量：", quality_items, 2, false, &quality_ok);
  if (!quality_ok) {
    return;
  }

  const bool kResetPaths = false;
  options_.ResetOptions(kResetPaths);

  if (data_item == "单张图像") {
    options_.ModifyForIndividualData();
  } else if (data_item == "视频帧") {
    options_.ModifyForVideoData();
  } else if (data_item == "网络图像") {
    options_.ModifyForInternetData();
  } else {
    LOG(FATAL_THROW) << "数据类型不存在";
  }

  if (quality_item == "低") {
    options_.ModifyForLowQuality();
  } else if (quality_item == "中") {
    options_.ModifyForMediumQuality();
  } else if (quality_item == "高") {
    options_.ModifyForHighQuality();
  } else if (quality_item == "极高") {
    options_.ModifyForExtremeQuality();
  } else {
    LOG(FATAL_THROW) << "质量级别不存在";
  }
}

void MainWindow::ResetOptions() {
  const bool kResetPaths = false;
  options_.ResetOptions(kResetPaths);
}

void MainWindow::SetLogOptions() {
  QDialog dialog(this);
  dialog.setWindowTitle("日志选项");
  dialog.resize(220, 160);

  QFormLayout* form_layout = new QFormLayout(&dialog);

  // Log target
  QComboBox* log_target_box = new QComboBox(&dialog);
  log_target_box->addItems({"stderr", "stdout", "file", "stderr_and_file"});
  log_target_box->setCurrentText(GetLogTarget().c_str());
  log_target_box->setToolTip(
      QString("默认输出路径：系统临时目录（通常为：%1）")
          .arg(std::filesystem::temp_directory_path().c_str()));
  form_layout->addRow("日志目标", log_target_box);

  // NOTE: glog resolves log file paths during initialization.
  // Runtime modification of FLAGS_log_dir does not re-open log files,
  // therefore directory changes are not supported here.

  // Verbosity
  QSpinBox* verbosity_box = new QSpinBox(&dialog);
  verbosity_box->setRange(0, 10);
  verbosity_box->setValue(FLAGS_v);
  form_layout->addRow("Verbosity", verbosity_box);

  // Minimum severity
  QComboBox* min_severity_box = new QComboBox(&dialog);
  min_severity_box->addItems({"INFO", "WARNING", "ERROR", "FATAL"});
  min_severity_box->setCurrentIndex(FLAGS_minloglevel);
  form_layout->addRow("最低日志级别", min_severity_box);

  // Color
#if COLMAP_GLOG_HAS_COLOR_SUPPORT
  QComboBox* color_box = new QComboBox(&dialog);
  color_box->addItems({"禁用", "启用"});
  color_box->setCurrentIndex(static_cast<int>(FLAGS_colorlogtostderr));
  form_layout->addRow("彩色日志", color_box);
#endif

  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);

  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  form_layout->addRow(buttons);

  if (dialog.exec() == QDialog::Accepted) {
    ApplyLogOptions(log_target_box->currentText().toStdString(),
                    verbosity_box->value(),
                    min_severity_box->currentIndex(),
#if COLMAP_GLOG_HAS_COLOR_SUPPORT
                    color_box->currentIndex()
#else
                    0
#endif
    );
  }
}

void MainWindow::About() {
  QMessageBox message_box(this);
  message_box.setWindowTitle(tr("关于"));
  message_box.setTextFormat(Qt::RichText);

  // Render the logo directly from the SVG at the target size (accounting for
  // high-DPI displays) so it stays crisp instead of scaling a raster.
  constexpr int kLogoSize = 96;
  const qreal device_pixel_ratio = devicePixelRatioF();
  QPixmap logo(QSize(kLogoSize, kLogoSize) * device_pixel_ratio);
  logo.fill(Qt::transparent);
  QSvgRenderer logo_renderer(QStringLiteral(":/media/colmap-logo.svg"));
  QPainter logo_painter(&logo);
  logo_renderer.render(&logo_painter);
  logo_painter.end();
  logo.setDevicePixelRatio(device_pixel_ratio);
  message_box.setIconPixmap(logo);

  message_box.setText(QString::fromStdString(GetVersionInfo()));
  message_box.setInformativeText(QString().asprintf(
      "<small>%s</small><br><br>COLMAP 是一款通用的运动恢复结构（SfM）与多视图立体（MVS）流程工具。<br><br>COLMAP 由核心维护者与众多社区贡献者共同开发。文档、源代码及完整贡献者名单请访问 <a href=\"https://colmap.github.io/\">https://colmap.github.io/</a>。",
      GetBuildInfo().c_str()));
  message_box.exec();
}

void MainWindow::Documentation() {
  QDesktopServices::openUrl(QUrl("https://colmap.github.io/"));
}

void MainWindow::Support() {
  QDesktopServices::openUrl(
      QUrl("https://github.com/colmap/colmap/discussions"));
}

void MainWindow::License() {
  QFile file(":/COPYING.txt");
  QString license;
  if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    // Reflow the raw license text: ignore the hard line breaks within each
    // paragraph and only break at blank lines, rendering paragraphs with
    // spacing between them.
    const QString text = QString::fromUtf8(file.readAll());
    const QStringList paragraphs =
        text.split(QRegularExpression("\n\\s*\n"), Qt::SkipEmptyParts);
    for (const QString& paragraph : paragraphs) {
      license += "<p>" + paragraph.simplified().toHtmlEscaped() + "</p>";
    }
  } else {
    license = tr("加载许可文本失败。");
  }

  QMessageBox message_box(this);
  message_box.setWindowTitle(tr("许可协议"));
  message_box.setTextFormat(Qt::RichText);
  message_box.setText(license);
  message_box.setStyleSheet("QLabel { font-weight: normal; }");
  message_box.exec();
}

void MainWindow::RenderToggle() {
  if (render_options_widget_->automatic_update) {
    render_options_widget_->automatic_update = false;
    render_options_widget_->counter = 0;
    action_render_toggle_->setIcon(ThemedIcon(":/media/render-disabled.svg"));
    action_render_toggle_->setText(tr("启用渲染"));
  } else {
    render_options_widget_->automatic_update = true;
    render_options_widget_->counter = 0;
    RenderNow();
    action_render_toggle_->setIcon(ThemedIcon(":/media/render-enabled.svg"));
    action_render_toggle_->setText(tr("禁用渲染"));
  }
}

void MainWindow::UpdateTimer() {
  const int elapsed_time = static_cast<int>(timer_.ElapsedSeconds());
  const int seconds = elapsed_time % 60;
  const int minutes = (elapsed_time / 60) % 60;
  const int hours = (elapsed_time / 3600) % 24;
  const int days = elapsed_time / 86400;
  statusbar_timer_label_->setText(QString().asprintf(
      "时间 %02d:%02d:%02d:%02d", days, hours, minutes, seconds));
}

void MainWindow::ShowInvalidProjectError() {
  QMessageBox::critical(this,
                        "",
                        tr("您必须先创建有效项目：<i>文件 > 新建项目</i> 或 <i>文件 > 编辑项目</i>"));
}

void MainWindow::EnableBlockingActions() {
  for (auto& action : blocking_actions_) {
    action->setEnabled(true);
  }
  UpdateMapperControls();
}

void MainWindow::UpdateMapperControls() {
  // Derive the enabled state of the step/pause controls from the reconstruction
  // lifecycle rather than from the controls' current state. This keeps them
  // correct when the user toggles the mapper type back and forth (e.g. switch
  // to hierarchical and back to incremental), which the previous
  // preserve-current-state logic got wrong by permanently disabling stepping.
  const bool started = mapper_controller_ && mapper_controller_->IsStarted();
  const bool finished = mapper_controller_ && mapper_controller_->IsFinished();
  const bool paused = mapper_controller_ && mapper_controller_->IsPaused();
  const bool running = started && !paused && !finished;

  // The hierarchical mapper runs as a single solve that cannot be stepped or
  // paused, so both controls stay disabled while it is selected.
  const bool supports_stepping = mapper_type_ != MapperType::HIERARCHICAL;

  // Stepping is available while the reconstruction is idle or paused (but not
  // while running or finished); pausing is only available while running.
  action_reconstruction_step_->setEnabled(supports_stepping && !running &&
                                          !finished);
  action_reconstruction_pause_->setEnabled(supports_stepping && running);
}

void MainWindow::DisableBlockingActions() {
  for (auto& action : blocking_actions_) {
    action->setDisabled(true);
  }
}

void MainWindow::UpdateWindowTitle() {
  if (options_.project_path->empty()) {
    setWindowTitle(QString::fromStdString("COLMAP"));
  } else {
    std::string project_title = options_.project_path->string();
    if (project_title.size() > 80) {
      project_title =
          "..." + project_title.substr(project_title.size() - 77, 77);
    }
    setWindowTitle(QString::fromStdString("COLMAP - " + project_title));
  }
}

}  // namespace colmap
