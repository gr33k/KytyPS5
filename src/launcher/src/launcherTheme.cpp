#include "launcherTheme.h"

#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QObject>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>

namespace {

QString g_default_style;
QPalette g_default_palette;
bool     g_defaults_captured = false;

QPalette DarkPalette() {
	QPalette palette;
	palette.setColor(QPalette::Window, QColor(53, 53, 53));
	palette.setColor(QPalette::WindowText, Qt::white);
	palette.setColor(QPalette::Base, QColor(25, 25, 25));
	palette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
	palette.setColor(QPalette::ToolTipBase, Qt::white);
	palette.setColor(QPalette::ToolTipText, Qt::white);
	palette.setColor(QPalette::Text, Qt::white);
	palette.setColor(QPalette::Button, QColor(53, 53, 53));
	palette.setColor(QPalette::ButtonText, Qt::white);
	palette.setColor(QPalette::BrightText, Qt::red);
	palette.setColor(QPalette::Link, QColor(42, 130, 218));
	palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
	palette.setColor(QPalette::HighlightedText, Qt::white);
	palette.setColor(QPalette::PlaceholderText, QColor(127, 127, 127));

	palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
	palette.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
	palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
	palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(80, 80, 80));
	palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(127, 127, 127));
	return palette;
}

class PaletteChangeFilter final: public QObject {
public:
	explicit PaletteChangeFilter(QApplication& application)
	    : QObject(&application), m_application(application) {}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override {
		if (event->type() == QEvent::ApplicationPaletteChange && !m_refresh_pending) {
			m_refresh_pending = true;
			QTimer::singleShot(0, &m_application, [this] {
				m_application.setStyleSheet(m_application.styleSheet());
				m_refresh_pending = false;
			});
		}

		return QObject::eventFilter(watched, event);
	}

private:
	QApplication& m_application;
	bool          m_refresh_pending = false;
};

} // namespace

namespace LauncherTheme {

void Initialize(QApplication& application) {
	if (!g_defaults_captured) {
		g_default_style     = application.style() != nullptr
		                          ? application.style()->objectName()
		                          : QStringLiteral("Fusion");
		g_default_palette   = application.palette();
		g_defaults_captured = true;
	}
	QFile style_sheet_file(QStringLiteral(":/styles/launcher.qss"));
	if (style_sheet_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		application.setStyleSheet(QString::fromUtf8(style_sheet_file.readAll()));
		application.installEventFilter(new PaletteChangeFilter(application));
	}
}

void ApplyTheme(Theme theme) {
	auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
	if (application == nullptr) {
		return;
	}

	switch (theme) {
		case Theme::Dark:
			application->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
			application->setPalette(DarkPalette());
			break;
		case Theme::Light:
			application->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
			application->setPalette(application->style()->standardPalette());
			break;
		case Theme::System:
			if (auto* style = QStyleFactory::create(g_default_style); style != nullptr) {
				application->setStyle(style);
			}
			application->setPalette(g_default_palette);
			break;
	}
}

} // namespace LauncherTheme
