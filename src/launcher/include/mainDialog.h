#ifndef MAIN_DIALOG_H
#define MAIN_DIALOG_H

#include <QMainWindow>
#include <QString>

class QWidget;
class QProcess;
class MainDialogPrivate;
class QSettings;
class QResizeEvent;
class QCloseEvent;
class QEvent;

class Configuration;
class MainDialog: public QMainWindow {
	Q_OBJECT

signals:
	void Start();
	void Resize();

public:
	explicit MainDialog(QWidget* parent = nullptr);
	~MainDialog() override = default;

	void RunInterpreter(QProcess* process, const Configuration& info);

	static void WriteSettings(QSettings& s);
	static void ReadSettings(QSettings& s);

	void resizeEvent(QResizeEvent* event) override;
	void changeEvent(QEvent* event) override;
	void closeEvent(QCloseEvent* event) override;

private:
	MainDialogPrivate* m_p = nullptr;
};

#endif // MAIN_DIALOG_H
