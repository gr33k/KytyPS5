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
	void ViewTrophyOverview();
	int GetColumnCount() const;
	int GetGameCount() const;
	QString GetColumnTitle(int column) const;
	bool IsColumnVisible(int column) const;
	void SetColumnVisible(int column, bool visible);
	bool IsGridView() const;
	int GridIconWidth() const;
	void SetGridView(bool enabled);
	void SetGridSize(int width);

signals:

	void Run();
	void Select();
	void PreviewControllerColor(const QString& color);

protected:
	void changeEvent(QEvent* event) override;

public slots:
	void WriteSettings();
	void ReadSettings();

public slots:

	void edit_configuration();
	void delete_configuartion();
	void edit_global_settings();
	void edit_input_mapping();
	void list_itemDoubleClicked(QTreeWidgetItem* witem, int column);
	void open_game_folder();
	void remove_save_data();
	void compress_game();
	void filter_configurations(const QString& text);
	void EditPatches();

private:
	void               SelectItem(QTreeWidgetItem* witem);
	void               ShowContextMenu(QTreeWidgetItem* witem);
	void               ApplyCompatibility();
	void               UpdateToolbarIcons();
	void               ImportGameSettings(QWidget* parent);
	void               ExportGameSettings(QWidget* parent) const;
	[[nodiscard]] bool HasValidGameDirectory() const;

	ConfigurationItem*            m_selected_item = nullptr;
	bool                          m_run_enabled   = true;
	Ui::ConfigurationListWidget*  m_ui            = nullptr;
	QString                       m_settings_file;
	QString                       m_runtime_directory;
	QStringList                   m_game_dirs;
	QString                       m_addons_dir;
	QMap<QString, QStringList>    m_addcont; // User add-ons by title ID.
	Configuration                 m_global_info;
	QMap<QString, Configuration*> m_custom_infos;
	CompatibilityDatabase*        m_compatibility = nullptr;
	ControllerPreview*            m_controller_preview = nullptr;
};

#endif // CONFIGURATION_LIST_WIDGET_H
