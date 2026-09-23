#ifndef LAUNCHER_THEME_H
#define LAUNCHER_THEME_H

class QApplication;

namespace LauncherTheme {

enum class Theme {
	System,
	Light,
	Dark,
};

void Initialize(QApplication& application);
void ApplyTheme(Theme theme);

} // namespace LauncherTheme

#endif // LAUNCHER_THEME_H
