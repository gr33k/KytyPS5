#include "mainDialog.h"

#include "configuration.h"
#include "configurationItem.h"
#include "configurationListWidget.h"
#include "launcherTheme.h"
#include "patchesDialog.h"
#include "trophyViewerDialog.h"
#include "updateChecker.h"

#include <QApplication>
#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QMenuBar>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QStatusBar>
#include <QStyle>
#include <QTextStream>
#include <QToolBar>
#include <QUrl>
#include <QVariant>
#include <QtCore>

#include <cstdint>

#include "ui_main_dialog.h"

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>

class QWidget;

#if defined(_WIN32)
constexpr char EMULATOR_EXE[] = "kyty_emulator.exe";
#else
constexpr char EMULATOR_EXE[] = "kyty_emulator";
#endif

#if defined(__linux__)
constexpr char KYTY_BASH_FILE[] = "kyty_run.sh";
#endif
constexpr char SETTINGS_MAIN_DIALOG[]        = "MainDialog";
constexpr char SETTINGS_MAIN_LAST_GEOMETRY[] = "geometry";
constexpr char SETTINGS_MAIN_LAST_STATE[]    = "state";
constexpr char SETTINGS_CHECK_UPDATES[]      = "check_updates_on_startup";
constexpr char SETTINGS_THEME[]              = "theme";

class MainDialogPrivate: public QObject {
	Q_OBJECT

public:
	explicit MainDialogPrivate(QObject* parent = nullptr): QObject(parent) {}
	~MainDialogPrivate() override;

	void Setup(MainDialog* main_dialog);
	void SaveWindowState();
	void ShowAbout();
	void UpdateToolbarIcons();
	QString CreateLogFile(const Configuration& info);
	void OpenLogFolder();
	void ShowFailureDialog(int exitCode, QProcess::ExitStatus exitStatus);

	/*slots:*/

	void Update();
	void FindInterpreter();
	void Run();

	[[nodiscard]] const QString& GetInterpreter() const { return m_interpreter; }

	static void WriteSettings(QSettings& s);
	static void ReadSettings(QSettings& s);

private:
	void BuildChrome();
	void RebuildColumnsMenu();

	static QByteArray g_last_geometry;
	static QByteArray g_last_state;
	static bool       g_check_updates_on_startup;
	static int        g_theme;

	Ui::MainDialog* m_ui             = {nullptr};
	MainDialog*     m_main_dialog    = nullptr;
	UpdateChecker*  m_update_checker = nullptr;
	QString         m_interpreter;
	QString         m_version;

	QProcess m_process;
	bool     m_expect_kill  = false;
	QString  m_last_log_path;

	QPointer<ConfigurationItem> m_running_item;

	QToolBar* m_toolbar              = nullptr;
	QAction*  m_action_rescan        = nullptr;
	QAction*  m_action_global        = nullptr;
	QAction*  m_action_inputs        = nullptr;
	QAction*  m_action_run           = nullptr;
	QAction*  m_action_edit          = nullptr;
	QAction*  m_action_delete        = nullptr;
	QAction*  m_action_open_folder   = nullptr;
	QAction*  m_action_patches       = nullptr;
	QAction*  m_action_trophies      = nullptr;
	QAction*  m_action_updates       = nullptr;
	QAction*  m_action_check_startup = nullptr;
	QMenu*    m_menu_columns         = nullptr;
	QLineEdit* m_search_edit         = nullptr;
	QLabel*   m_status_games         = nullptr;
	QLabel*   m_status_emulator      = nullptr;
};

QByteArray MainDialogPrivate::g_last_geometry;
QByteArray MainDialogPrivate::g_last_state;
bool       MainDialogPrivate::g_check_updates_on_startup = true;
int        MainDialogPrivate::g_theme                   = 0;

MainDialog::MainDialog(QWidget* parent): QMainWindow(parent), m_p(new MainDialogPrivate(this)) {
	m_p->Setup(this);
}

MainDialogPrivate::~MainDialogPrivate() {
	delete m_ui;
}

void MainDialogPrivate::Setup(MainDialog* main_dialog) {
	m_ui = new Ui::MainDialog;
	m_ui->setupUi(main_dialog);

	m_main_dialog    = main_dialog;
	m_update_checker = new UpdateChecker(main_dialog);

	BuildChrome();
	m_action_updates->setVisible(UpdateChecker::IsSupported());
	m_action_check_startup->setVisible(UpdateChecker::IsSupported());

	connect(main_dialog, &MainDialog::Start, this, &MainDialogPrivate::FindInterpreter,
	        Qt::QueuedConnection);
	connect(m_ui->widget, &ConfigurationListWidget::Select, this, &MainDialogPrivate::Update);
	connect(m_ui->widget, &ConfigurationListWidget::Run, this, &MainDialogPrivate::Run);
	connect(m_update_checker, &UpdateChecker::CheckingChanged, m_action_updates,
	        &QAction::setDisabled);
	connect(main_dialog, &MainDialog::Resize, this,
	        [this]() { SaveWindowState(); });

	connect(&m_process,
	        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
	        [this](int exitCode, QProcess::ExitStatus exitStatus) {
		        const bool failed =
		            (exitStatus == QProcess::CrashExit || exitCode != 0);
		        const bool expect_kill = m_expect_kill;
		        m_expect_kill          = false;
		        QPointer<ConfigurationItem> finished_item = m_running_item;
		        if (m_running_item != nullptr) {
			        m_running_item->SetRunning(false);
		        }
		        Update();
		        if (failed && !expect_kill && finished_item != nullptr) {
			        ShowFailureDialog(exitCode, exitStatus);
		        }
	        });

	m_main_dialog->restoreGeometry(g_last_geometry);
	m_main_dialog->restoreState(g_last_state);

	Update();
}

void MainDialogPrivate::BuildChrome() {
	auto* window = m_main_dialog;

	m_action_run = new QAction(window->style()->standardIcon(QStyle::SP_MediaPlay),
	                           tr("&Run"), window);
	m_action_run->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
	m_action_run->setStatusTip(tr("Run the selected game"));
	connect(m_action_run, &QAction::triggered, this, &MainDialogPrivate::Run);

	auto* action_add_folder = new QAction(
	    window->style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Add Game &Folder..."), window);
	action_add_folder->setShortcut(QKeySequence::Open);
	action_add_folder->setStatusTip(tr("Manage game folders in global settings"));
	connect(action_add_folder, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::edit_global_settings);

	auto* action_rescan = new QAction(QIcon(QStringLiteral(":/icons/refresh.svg")),
	                                  tr("&Rescan Game List"), window);
	action_rescan->setShortcuts(QKeySequence::Refresh);
	action_rescan->setStatusTip(tr("Rescan game folders for new games"));
	connect(action_rescan, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::ScanGameDirectory);
	m_action_rescan = action_rescan;

	auto* action_exit = new QAction(tr("E&xit"), window);
	action_exit->setShortcut(QKeySequence::Quit);
	action_exit->setStatusTip(tr("Quit the launcher"));
	connect(action_exit, &QAction::triggered, window, &QWidget::close);

	m_action_edit = new QAction(QIcon(QStringLiteral(":/icons/edit-configuration.svg")),
	                            tr("&Edit Game Settings..."), window);
	m_action_edit->setStatusTip(tr("Edit the selected game's settings"));
	connect(m_action_edit, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::edit_configuration);

	m_action_delete = new QAction(QIcon(QStringLiteral(":/icons/remove-configuration.svg")),
	                              tr("&Clear Custom Settings"), window);
	m_action_delete->setShortcut(QKeySequence::Delete);
	m_action_delete->setStatusTip(tr("Clear the selected game's custom settings"));
	connect(m_action_delete, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::delete_configuartion);

	m_action_open_folder =
	    new QAction(window->style()->standardIcon(QStyle::SP_DirOpenIcon),
	                tr("&Open Game Folder"), window);
	m_action_open_folder->setStatusTip(tr("Open the selected game's folder"));
	connect(m_action_open_folder, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::open_game_folder);

	m_action_trophies = new QAction(
	    window->style()->standardIcon(QStyle::SP_FileDialogContentsView),
	    tr("View &Trophies..."), window);
	m_action_trophies->setStatusTip(tr("View the selected game's trophies"));
	connect(m_action_trophies, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::ViewTrophies);

	m_action_patches = new QAction(tr("&Cheats (experimental)..."), window);
	m_action_patches->setStatusTip(tr("Edit cheats for the selected game"));
	connect(m_action_patches, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::EditPatches);

	auto* action_global = new QAction(QIcon(QStringLiteral(":/icons/global-settings.svg")),
	                                  tr("&Global Settings..."), window);
	action_global->setStatusTip(tr("Edit global settings and game folders"));
	connect(action_global, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::edit_global_settings);

	auto* action_inputs = new QAction(QIcon(QStringLiteral(":/icons/input-mapping.svg")),
	                                  tr("&Input Mapping..."), window);
	action_inputs->setStatusTip(tr("Edit the global input mapping"));
	connect(action_inputs, &QAction::triggered, m_ui->widget,
	        &ConfigurationListWidget::edit_input_mapping);
	m_action_global = action_global;
	m_action_inputs = action_inputs;

	auto* action_find = new QAction(tr("&Find Game"), window);
	action_find->setShortcut(QKeySequence::Find);
	action_find->setStatusTip(tr("Search the game list"));
	connect(action_find, &QAction::triggered, this, [this]() {
		if (m_search_edit != nullptr) {
			m_search_edit->setFocus(Qt::ShortcutFocusReason);
			m_search_edit->selectAll();
		}
	});

	m_action_check_startup =
	    new QAction(tr("Check for updates on startup"), window);
	m_action_check_startup->setCheckable(true);
	m_action_check_startup->setChecked(g_check_updates_on_startup);
	connect(m_action_check_startup, &QAction::toggled, this, [this](bool checked) {
		g_check_updates_on_startup = checked;
		m_ui->widget->WriteSettings();
	});

	m_action_updates = new QAction(tr("Check for &Updates"), window);
	m_action_updates->setStatusTip(tr("Check for launcher updates"));
	connect(m_action_updates, &QAction::triggered, this,
	        [this]() { m_update_checker->Check(true); });

	auto* action_about = new QAction(tr("&About KytyPS5"), window);
	connect(action_about, &QAction::triggered, this, &MainDialogPrivate::ShowAbout);

	auto* action_about_qt = new QAction(tr("About &Qt"), window);
	connect(action_about_qt, &QAction::triggered, window,
	        [window]() { QMessageBox::aboutQt(window); });

	auto* menu_file = window->menuBar()->addMenu(tr("&File"));
	menu_file->addAction(action_add_folder);
	menu_file->addAction(action_rescan);
	menu_file->addSeparator();
	menu_file->addAction(action_exit);

	auto* menu_game = window->menuBar()->addMenu(tr("&Game"));
	menu_game->addAction(m_action_run);
	menu_game->addSeparator();
	menu_game->addAction(m_action_open_folder);
	menu_game->addAction(m_action_trophies);
	menu_game->addAction(m_action_patches);
	menu_game->addSeparator();
	menu_game->addAction(m_action_edit);
	menu_game->addAction(m_action_delete);
	menu_game->addSeparator();

	auto* action_log_folder =
	    new QAction(window->style()->standardIcon(QStyle::SP_DirIcon), tr("Open Log &Folder"), window);
	action_log_folder->setStatusTip(tr("Open the folder with per-game emulator logs"));
	connect(action_log_folder, &QAction::triggered, this,
	        &MainDialogPrivate::OpenLogFolder);
	menu_game->addAction(action_log_folder);

	auto* menu_tools = window->menuBar()->addMenu(tr("&Tools"));
	menu_tools->addAction(action_global);
	menu_tools->addAction(action_inputs);

	auto* menu_view = window->menuBar()->addMenu(tr("&View"));
	menu_view->addAction(action_find);
	menu_view->addSeparator();
	menu_view->addAction(m_action_check_startup);

	auto* menu_theme = menu_view->addMenu(tr("&Theme"));
	auto* theme_group = new QActionGroup(window);
	theme_group->setExclusive(true);
	const auto add_theme_action = [&](const QString& text, LauncherTheme::Theme theme) {
		auto* action = menu_theme->addAction(text);
		action->setCheckable(true);
		action->setActionGroup(theme_group);
		action->setChecked(g_theme == static_cast<int>(theme));
		connect(action, &QAction::triggered, this, [this, theme] {
			g_theme = static_cast<int>(theme);
			LauncherTheme::ApplyTheme(theme);
			m_ui->widget->WriteSettings();
		});
	};
	add_theme_action(tr("&System"), LauncherTheme::Theme::System);
	add_theme_action(tr("&Light"), LauncherTheme::Theme::Light);
	add_theme_action(tr("&Dark"), LauncherTheme::Theme::Dark);

	m_menu_columns = menu_view->addMenu(tr("&Columns"));
	connect(m_menu_columns, &QMenu::aboutToShow, this,
	        &MainDialogPrivate::RebuildColumnsMenu);

	auto* menu_help = window->menuBar()->addMenu(tr("&Help"));
	menu_help->addAction(m_action_updates);
	menu_help->addSeparator();
	menu_help->addAction(action_about);
	menu_help->addAction(action_about_qt);

	m_toolbar = window->addToolBar(tr("Main"));
	m_toolbar->setObjectName(QStringLiteral("main_toolbar"));
	m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	m_toolbar->addAction(m_action_run);
	m_toolbar->addAction(action_rescan);
	m_toolbar->addSeparator();
	m_toolbar->addAction(action_add_folder);
	m_toolbar->addAction(action_global);
	m_toolbar->addAction(action_inputs);

	auto* toolbar_spacer = new QWidget(window);
	toolbar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	m_toolbar->addWidget(toolbar_spacer);

	m_search_edit = new QLineEdit(window);
	m_search_edit->setObjectName(QStringLiteral("search_line_edit"));
	m_search_edit->setClearButtonEnabled(true);
	m_search_edit->setMaximumWidth(300);
	m_search_edit->setPlaceholderText(tr("Search name or serial"));
	m_search_edit->setToolTip(tr("Search game name or serial"));
	connect(m_search_edit, &QLineEdit::textChanged, m_ui->widget,
	        &ConfigurationListWidget::filter_configurations);
	m_toolbar->addWidget(m_search_edit);

	UpdateToolbarIcons();

	menu_view->addSeparator();
	menu_view->addAction(m_toolbar->toggleViewAction());

	m_status_games    = new QLabel(window);
	m_status_games->setTextFormat(Qt::RichText);
	m_status_games->setAlignment(Qt::AlignVCenter);
	m_status_games->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_status_emulator = new QLabel(window);
	m_status_emulator->setAlignment(Qt::AlignVCenter);
	m_status_emulator->setTextInteractionFlags(Qt::TextSelectableByMouse);
	window->statusBar()->addWidget(m_status_games, 1);
	window->statusBar()->addPermanentWidget(m_status_emulator);

	auto* action_status_bar = new QAction(tr("Status Bar"), window);
	action_status_bar->setCheckable(true);
	action_status_bar->setChecked(true);
	connect(action_status_bar, &QAction::toggled, window->statusBar(), &QWidget::setVisible);
	menu_view->addAction(action_status_bar);
}

void MainDialogPrivate::SaveWindowState() {
	g_last_geometry = m_main_dialog->saveGeometry();
	g_last_state    = m_main_dialog->saveState();
	m_ui->widget->WriteSettings();
}

void MainDialogPrivate::UpdateToolbarIcons() {
	if (m_toolbar == nullptr) {
		return;
	}
	const auto color = m_main_dialog->palette().color(QPalette::Window).lightness() < 128
	                       ? QColor(Qt::white)
	                       : QColor(Qt::black);
	const qreal dpr        = m_main_dialog->devicePixelRatioF();
	const QSize icon_size  = m_toolbar->iconSize();
	const auto  tinted     = [&](const QString& resource) {
        auto pixmap = QIcon(resource).pixmap(icon_size, dpr);
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        return QIcon(pixmap);
	};

	if (m_action_rescan != nullptr) {
		m_action_rescan->setIcon(tinted(QStringLiteral(":/icons/refresh.svg")));
	}
	if (m_action_global != nullptr) {
		m_action_global->setIcon(tinted(QStringLiteral(":/icons/global-settings.svg")));
	}
	if (m_action_inputs != nullptr) {
		m_action_inputs->setIcon(tinted(QStringLiteral(":/icons/input-mapping.svg")));
	}
	if (m_action_edit != nullptr) {
		m_action_edit->setIcon(tinted(QStringLiteral(":/icons/edit-configuration.svg")));
	}
	if (m_action_delete != nullptr) {
		m_action_delete->setIcon(tinted(QStringLiteral(":/icons/remove-configuration.svg")));
	}
}

void MainDialogPrivate::RebuildColumnsMenu() {
	m_menu_columns->clear();
	const int count = m_ui->widget->GetColumnCount();
	for (int section = 0; section < count; section++) {
		auto* action = m_menu_columns->addAction(m_ui->widget->GetColumnTitle(section));
		action->setCheckable(true);
		action->setChecked(m_ui->widget->IsColumnVisible(section));
		connect(action, &QAction::toggled, this, [this, section](bool visible) {
			m_ui->widget->SetColumnVisible(section, visible);
			m_ui->widget->WriteSettings();
		});
	}
}

void MainDialogPrivate::ShowAbout() {
	const QString version = m_version.isEmpty() ? tr("unknown") : m_version;
	QMessageBox::about(m_main_dialog, tr("About KytyPS5"),
	                   tr("<h3>KytyPS5 Launcher</h3>"
	                      "<p>Version: %1</p>"
	                      "<p>A free and open-source PlayStation 5 emulator.</p>"
	                      "<p>Not affiliated with Sony Interactive Entertainment.</p>")
	                       .arg(version));
}

void MainDialogPrivate::FindInterpreter() {
	QDir search_dir(QApplication::applicationDirPath());
	m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);

	if (!QFile::exists(m_interpreter)) {
		search_dir.cdUp();
		m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);
	}

	bool found = QFile::exists(m_interpreter);

	if (found) {
		m_status_emulator->setText(tr("Emulator: ") + m_interpreter);

		QProcess test;
		test.setProgram(m_interpreter);
		test.start();
		test.waitForFinished();

		auto output = QString(test.readAllStandardOutput());
		auto lines  = output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

		if (lines.count() >= 2) {
			m_version = lines.at(0).startsWith("exe_name") ? lines.at(1) : lines.at(0);
			QString ver_short = m_version;
			const QRegularExpression ver_re(QStringLiteral("ver\\s*=\\s*([^,]+)"));
			const auto ver_match = ver_re.match(m_version);
			if (ver_match.hasMatch()) {
				ver_short = ver_match.captured(1).trimmed();
			}
			m_status_emulator->setText(tr("Version: %1").arg(ver_short));
			m_status_emulator->setToolTip(m_interpreter + QStringLiteral("\n") + m_version);
		} else {
			found = false;
		}
	}

	if (!found) {
		QMessageBox::critical(m_main_dialog, tr("Error"), tr("Can't find emulator"));
		QApplication::quit();
		return;
	}

	// Prompt for a game folder when none are configured, but keep the launcher
	// open if the user dismisses the dialog (quitting here can segfault during
	// nested modal shutdown / background compatibility load).
	m_ui->widget->EnsureGameDirectory();

	Update();
	if (g_check_updates_on_startup) {
		m_update_checker->Check(false);
	}
}

static QString BoolArg(bool value) {
	return value ? QStringLiteral("true") : QStringLiteral("false");
}

static QStringList CreateEmulatorArgs(const Configuration& info) {
	QStringList args;
	auto        r = EnumToText(info.screen_resolution).split('x');

	if (r.size() != 2) {
		return {};
	}

	args << "--screen-width" << r.at(0);
	args << "--screen-height" << r.at(1);
	args << "--user-name" << info.user_name;
	args << "--user-id" << QString::number(info.user_id);
	if (!info.audio_input_device.isEmpty()) {
		args << "--mic" << info.audio_input_device;
	}
	args << "--present-mode" << EnumToText(info.present_mode);
	if (info.gpu_index >= 0) {
		args << "--gpu" << QString::number(info.gpu_index);
	}
	if (info.fullscreen_enabled) {
		args << "--fullscreen";
	}
	args << "--readback-linear-images" << BoolArg(info.readback_linear_images);
	if (info.tessellation_enabled) {
		args << "--tessellation";
	}
	args << "--vblank-frequency" << QString::number(info.vblank_frequency);
	args << "--console-language" << QString::number(info.console_language);
	args << "--vulkan-validation" << BoolArg(info.vulkan_validation_enabled);
	args << "--shader-validation" << BoolArg(info.shader_validation_enabled);
	args << "--shader-optimization-type" << EnumToText(info.shader_optimization_type);
	args << "--shader-log-direction" << EnumToText(info.shader_log_direction);
	args << "--shader-log-folder" << info.shader_log_folder;
	args << "--command-buffer-dump" << BoolArg(info.command_buffer_dump_enabled);
	args << "--command-buffer-dump-folder" << info.command_buffer_dump_folder;
	args << "--printf-direction" << EnumToText(info.printf_direction);
	args << "--printf-output-file" << info.printf_output_file;
	if (info.profiler_enabled) {
		args << "--profile";
	}
	args << "--spirv-debug-printf" << "false";
	if (info.amd_cpu_enabled) {
		args << "--amd-cpu";
	}
#if defined(_WIN32)
	if (info.red_zone_protection_enabled) {
		args << "--redzone";
	}
#endif
	for (const auto& binding: info.host_input_mapping) {
		args << "--keymap" << binding;
	}
	if (info.renderdoc_enabled) {
		args << "--rd";
	}

	QString game = info.basedir;
	if (!info.elf.isEmpty()) {
		game = QDir(info.basedir).filePath(info.elf);
	}
	args << "--game" << game;

	const auto patch_plan = PatchesDialog::PatchPlanPath(info.title_id);
	if (QFileInfo::exists(patch_plan)) {
		args << "--game-patch" << patch_plan;
	}

	return args;
}

#ifdef __linux__
static QString BashQuote(QString value) {
	value.replace('\'', "'\\''");
	return QStringLiteral("'") + value + QStringLiteral("'");
}

static bool CreateBashScript(const QString& interpreter, const QStringList& args,
                             const QString& file_name) {
	QFile file(file_name);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QTextStream s(&file);

		s << "#!/bin/bash\n";
		s << BashQuote(interpreter);
		for (const auto& arg: args) {
			s << " " << BashQuote(arg);
		}
		s << "\n";
		s << "echo Press any key...\n";
		s << "read -n1\n";

		file.close();

		return file.setPermissions(file.permissions() | QFile::ExeUser | QFile::ExeOwner |
		                           QFile::ExeGroup);
	}
	return false;
}

// Find a terminal and its command separator.
static bool FindTerminal(QString* program, QStringList* prefix) {
	struct TerminalSpec {
		const char* executable;
		const char* separator; // nullptr when the command follows immediately
	};

	static const TerminalSpec candidates[] = {
	    {"x-terminal-emulator", "-e"},
	    {"gnome-terminal", "--"},
	    {"konsole", "-e"},
	    {"xfce4-terminal", "-x"},
	    {"mate-terminal", "--"},
	    {"tilix", "-e"},
	    {"alacritty", "-e"},
	    {"kitty", nullptr},
	    {"foot", nullptr},
	    {"wezterm", "-e"},
	    {"urxvt", "-e"},
	    {"xterm", "-e"},
	};

	const auto try_candidate = [program, prefix](const QString& executable, const char* separator) {
		const auto resolved = QStandardPaths::findExecutable(executable);
		if (resolved.isEmpty()) {
			return false;
		}
		*program = resolved;
		prefix->clear();
		if (separator != nullptr) {
			*prefix << QString::fromLatin1(separator);
		}
		return true;
	};

	if (const auto from_env = qEnvironmentVariable("TERMINAL"); !from_env.isEmpty()) {
		// Reuse the known separator for an explicit terminal.
		const auto  env_name  = QFileInfo(from_env).fileName();
		const char* separator = "-e";
		for (const auto& candidate: candidates) {
			if (env_name == QLatin1String(candidate.executable)) {
				separator = candidate.separator;
				break;
			}
		}
		if (try_candidate(from_env, separator)) {
			return true;
		}
	}

	for (const auto& candidate: candidates) {
		if (try_candidate(QString::fromLatin1(candidate.executable), candidate.separator)) {
			return true;
		}
	}

	return false;
}
#endif

void MainDialog::RunInterpreter(QProcess* process, const Configuration& info) {
	const auto& interpreter = m_p->GetInterpreter();

	QFileInfo f(interpreter);
	auto      dir = f.absoluteDir();

	auto args = CreateEmulatorArgs(info);
	if (args.isEmpty()) {
		QMessageBox::critical(this, tr("Error"), tr("Invalid emulator configuration"));
		QApplication::quit();
		return;
	}

#ifdef __linux__
	auto bash_file_name = dir.filePath(KYTY_BASH_FILE);
	if (!CreateBashScript(interpreter, args, bash_file_name)) {
		QMessageBox::critical(this, tr("Error"), tr("Can't create file:\n") + bash_file_name);
		QApplication::quit();
		return;
	}

	{
		QString     terminal;
		QStringList terminal_prefix;
		// Pass the script as a file argument (not bash -c) so paths with spaces work.
		if (FindTerminal(&terminal, &terminal_prefix)) {
			process->setProgram(terminal);
			process->setArguments(terminal_prefix + QStringList {"bash", bash_file_name});
		} else {
			// Run without a terminal as a fallback.
			process->setProgram(QStringLiteral("bash"));
			process->setArguments({bash_file_name});
		}
	}
#elif defined(_WIN32)
	{
		// Launch the emulator directly with no console window. Output goes
		// to the per-game log file, and the process handle belongs to the
		// emulator itself so relaunching can stop a previous run precisely.
		process->setProgram(interpreter);
		process->setArguments(args);
		process->setProcessChannelMode(QProcess::MergedChannels);
		process->setStandardOutputFile(m_p->CreateLogFile(info));
	}
#else
	process->setProgram(interpreter);
	process->setArguments(args);
#endif
	process->setWorkingDirectory(dir.path());
#if defined(_WIN32)
	process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
		args->flags |= static_cast<uint32_t>(CREATE_NO_WINDOW);
	});
#endif
	process->start();
	// Report immediate launch failures on all platforms.
	if (!process->waitForStarted(5000)) {
		QMessageBox::critical(
		    this, tr("Error"),
		    tr("Failed to start:\n%1\n\n%2").arg(process->program(), process->errorString()));
		return;
	}
	process->waitForFinished(100);
}

void MainDialog::WriteSettings(QSettings& s) {
	MainDialogPrivate::WriteSettings(s);
}

void MainDialog::ReadSettings(QSettings& s) {
	MainDialogPrivate::ReadSettings(s);
}

void MainDialog::resizeEvent(QResizeEvent* event) {
	emit Resize();
	QMainWindow::resizeEvent(event);
}

void MainDialog::changeEvent(QEvent* event) {
	if (event->type() == QEvent::PaletteChange) {
		m_p->UpdateToolbarIcons();
	}
	QMainWindow::changeEvent(event);
}

void MainDialog::closeEvent(QCloseEvent* event) {
	m_p->SaveWindowState();
	QMainWindow::closeEvent(event);
}

void MainDialogPrivate::WriteSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	if (!g_last_geometry.isEmpty()) {
		s.setValue(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry);
	}
	if (!g_last_state.isEmpty()) {
		s.setValue(SETTINGS_MAIN_LAST_STATE, g_last_state);
	}
	s.setValue(SETTINGS_CHECK_UPDATES, g_check_updates_on_startup);
	s.setValue(SETTINGS_THEME, g_theme);

	s.endGroup();
}

void MainDialogPrivate::ReadSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	g_last_geometry = s.value(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry).toByteArray();
	g_last_state    = s.value(SETTINGS_MAIN_LAST_STATE, g_last_state).toByteArray();
	g_check_updates_on_startup = s.value(SETTINGS_CHECK_UPDATES, true).toBool();
	g_theme = s.value(SETTINGS_THEME, 0).toInt();
	if (g_theme < 0 || g_theme > 2) {
		g_theme = 0;
	}
	LauncherTheme::ApplyTheme(static_cast<LauncherTheme::Theme>(g_theme));

	s.endGroup();
}

void MainDialogPrivate::Run() {
	// Starting a game while another is running stops the previous one
	// first, so a failed run can never block the next launch.
	if (m_process.state() != QProcess::NotRunning) {
		m_expect_kill = true;
		m_process.kill();
		m_process.waitForFinished(5000);
	}
	if (m_process.state() != QProcess::NotRunning) {
		m_expect_kill = false;
		QMessageBox::warning(m_main_dialog, tr("Run game"),
		                     tr("The previous game is still running and could not be stopped."));
		return;
	}

	m_running_item = m_ui->widget->GetSelectedItem();
	if (m_running_item == nullptr) {
		return;
	}

	m_running_item->SetRunning(true);

	auto info = m_ui->widget->CreateConfiguration(*m_running_item);
	m_main_dialog->RunInterpreter(&m_process, *info);

	if (m_process.state() == QProcess::NotRunning && m_running_item != nullptr) {
		// The emulator never started (see the launch error, if any).
		m_running_item->SetRunning(false);
		m_running_item = nullptr;
	}

	Update();
}

QString MainDialogPrivate::CreateLogFile(const Configuration& info) {
	QDir log_dir(
	    QFileInfo(m_interpreter).absoluteDir().filePath(QStringLiteral("_Logs")));
	log_dir.mkpath(QStringLiteral("."));

	QString sanitized;
	for (const auto ch: info.title_id.trimmed()) {
		sanitized += (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_'))
		                 ? ch
		                 : QLatin1Char('_');
	}
	if (sanitized.isEmpty()) {
		sanitized = QStringLiteral("last");
	}
	m_last_log_path = log_dir.filePath(sanitized + QStringLiteral(".log"));
	QFile::remove(m_last_log_path);
	return m_last_log_path;
}

void MainDialogPrivate::OpenLogFolder() {
	QDir log_dir(
	    QFileInfo(m_interpreter).absoluteDir().filePath(QStringLiteral("_Logs")));
	if (!log_dir.exists()) {
		log_dir.mkpath(QStringLiteral("."));
	}
	QDesktopServices::openUrl(QUrl::fromLocalFile(log_dir.absolutePath()));
}

void MainDialogPrivate::ShowFailureDialog(int exitCode, QProcess::ExitStatus exitStatus) {
	const QString reason =
	    (exitStatus == QProcess::CrashExit)
	        ? tr("The emulator crashed.")
	        : tr("The emulator exited with code %1.").arg(exitCode);
	QMessageBox box(QMessageBox::Warning, tr("Game failed"),
	                reason + tr("\n\nFull output was saved to:\n%1").arg(m_last_log_path),
	                QMessageBox::Ok, m_main_dialog);
	box.addButton(tr("Open Log Folder"), QMessageBox::ActionRole);
	box.exec();
	auto* clicked = box.clickedButton();
	if (clicked != nullptr && box.buttonRole(clicked) == QMessageBox::ActionRole) {
		OpenLogFolder();
	}
}

void MainDialogPrivate::Update() {
	const auto* item = m_ui->widget->GetSelectedItem();

	bool    run_enabled = (m_process.state() == QProcess::NotRunning && item != nullptr);
	bool    folder_open = false;
	bool    has_patches = false;
	bool    has_trophies = false;
	QString status_text;

	const int games = m_ui->widget->GetGameCount();
	status_text     = games == 1 ? tr("1 game") : tr("%1 games").arg(games);

	if (item != nullptr) {
		const auto& info = item->GetInfo();
		if (!info.basedir.isEmpty() && QDir(info.basedir).exists()) {
			folder_open = true;
		} else {
			run_enabled = false;
		}
		if (!info.name.isEmpty()) {
			const QString name   = info.name.toHtmlEscaped();
			const QString serial = info.title_id.toHtmlEscaped();
			if (item->IsRunning()) {
				const QString accent =
				    m_main_dialog->palette().color(QPalette::Highlight).name();
				status_text +=
				    tr("   •   <span style='color:%1'>●</span> <b>%2</b> (%3) — running")
				        .arg(accent, name, serial);
			} else if (serial.isEmpty()) {
				status_text += tr("   •   <b>%1</b>").arg(name);
			} else {
				status_text += tr("   •   <b>%1</b> (%2)").arg(name, serial);
			}
		}
		has_patches  = PatchesDialog::IsSupportedTitleId(info.title_id);
		has_trophies = TrophyViewerDialog::HasTrophyData(&info);
	}

	const bool item_busy = (item != nullptr && item->IsRunning());

	m_ui->widget->SetRunEnabled(run_enabled);
	m_action_run->setEnabled(run_enabled);
	m_action_edit->setEnabled(item != nullptr && !item_busy);
	m_action_delete->setEnabled(item != nullptr && !item_busy &&
	                            item->GetInfo().custom_settings);
	m_action_open_folder->setEnabled(folder_open);
	m_action_patches->setEnabled(item != nullptr && !item_busy && has_patches);
	m_action_trophies->setEnabled(item != nullptr && has_trophies);

	m_status_games->setText(status_text);
}

#include "mainDialog.moc"
