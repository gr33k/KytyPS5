#include "mainDialog.h"
#include "launcherTheme.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);
	QApplication::setApplicationDisplayName(QStringLiteral("KytyPS5 Launcher"));
	a.setDesktopFileName(QStringLiteral("KytyPS5"));
	a.setWindowIcon(QIcon(QStringLiteral(":/icons/app-icon.png")));
	LauncherTheme::Initialize(a);

	MainDialog w;

	w.emit Start();

	w.show();

	return QApplication::exec();
}
