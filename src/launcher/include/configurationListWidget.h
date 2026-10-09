#ifndef CONFIGURATION_LIST_WIDGET_H
#define CONFIGURATION_LIST_WIDGET_H

#include "configuration.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

class ConfigurationItem;
class CompatibilityDatabase;
class ControllerPreview;
class QEvent;
class QTreeWidgetItem;

namespace Ui {
class ConfigurationListWidget;
} // namespace Ui

class ConfigurationListWidget: public QWidget {
	Q_OBJECT

public:
	explicit ConfigurationListWidget(QWidget* parent = nullptr);
	~ConfigurationListWidget() override;

	void SetRunEnabled(bool flag) { m_run_enabled = flag; }
	void SetRuntimeDirectory(const QString& directory) { m_runtime_directory = directory; }
	void SetControllerPreview(ControllerPreview* preview) { m_controller_preview = preview; }

	[[nodiscard]] bool IsGridView() const { return m_grid_view; }
	[[nodiscard]] int  GridIconWidth() const { return m_grid_icon_width; }

	[[nodiscard]] const ConfigurationItem* GetSelectedItem() const { return m_selected_item; }
	ConfigurationItem*                     GetSelectedItem() { return m_selected_item; }

	[[nodiscard]] const QString& GetSettingsFile() const { return m_settings_file; }
	[[nodiscard]] const QString& GetGlobalControllerColor() const {
		return m_global_info.controller.color;
	}
	[[nodiscard]] std::unique_ptr<Configuration>
	CreateConfiguration(const ConfigurationItem& item) const;

	bool EnsureGameDirectory();
	void ScanGameDirectory();
	void ViewTrophies();
	void EditPatches();

	[[nodiscard]] int GetGameCount() const;
	[[nodiscard]] int GetColumnCount() const;
	[[nodiscard]] QString GetColumnTitle(int section) const;
	[[nodiscard]] bool IsColumnVisible(int section) const;
	void SetColumnVisible(int section, bool visible);

	void ViewTrophyOverview();


signals:

	void Run();
	void Select();
	void PreviewControllerColor(const QString& color);

public slots:
	void WriteSettings();
	void ReadSettings();
	void edit_configuration();
	void delete_configuartion();
	void edit_global_settings();
	void edit_input_mapping();

	void list_itemDoubleClicked(QTreeWidgetItem* witem, int column);

	void open_game_folder();
	void remove_save_data();
	void filter_configurations(const QString& text);
	void SetGridView(bool grid);
	void SetGridSize(int width);

protected slots:

	void show_context_menu(const QPoint& pos);

private:
	void               SelectItem(QTreeWidgetItem* witem);
	void               ShowContextMenu(QTreeWidgetItem* witem);
	void               ApplyCompatibility();
	void               ImportGameSettings(QWidget* parent);
	void               ExportGameSettings(QWidget* parent) const;
	[[nodiscard]] bool HasValidGameDirectory() const;

	ConfigurationItem*            m_selected_item = nullptr;
	bool                          m_run_enabled   = true;
	bool                          m_grid_view     = false;
	int                           m_grid_icon_width = 224;
	Ui::ConfigurationListWidget*  m_ui            = nullptr;
	QString                       m_settings_file;
	QString                       m_filter_text;
	QString                       m_runtime_directory;
	QStringList                   m_game_dirs;
	Configuration                 m_global_info;
	QMap<QString, Configuration*> m_custom_infos;
	CompatibilityDatabase*        m_compatibility = nullptr;
	ControllerPreview*            m_controller_preview = nullptr;
};

#endif // CONFIGURATION_LIST_WIDGET_H
