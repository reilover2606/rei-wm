#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QTabWidget>
#include <QTimer>
#include <QStandardPaths>

#include "mainwindow.h"

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setApplicationName("rei-settings");

	QCommandLineParser parser;
	parser.setApplicationDescription("Settings for the rei-wm compositor");
	parser.addHelpOption();
	QCommandLineOption config({ "c", "config" }, "Config file to edit (default: ~/.config/rei-wm/config.toml).", "path");
	QCommandLineOption wm("rei-wm", "Path to the rei-wm binary, used to check settings before saving.", "path");
	QCommandLineOption shot("screenshot", "Save a picture of the window to this file and exit (for documentation).", "file");
	QCommandLineOption tab("tab", "With --screenshot: the tab to show (0-based).", "n", "0");
	parser.addOption(shot);
	parser.addOption(tab);
	parser.addOption(config);
	parser.addOption(wm);
	parser.process(app);

	QString path = parser.value(config);
	if (path.isEmpty()) {
		QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
		if (base.isEmpty())
			base = QDir::homePath() + "/.config";
		path = base + "/rei-wm/config.toml";
	}

	MainWindow w(path, parser.value(wm));
	w.show();
	if (parser.isSet(shot)) {
		if (auto *tabs = w.findChild<QTabWidget *>("tabs"))
			tabs->setCurrentIndex(parser.value(tab).toInt());
		QTimer::singleShot(300, [&] {
			bool ok = w.grab().save(parser.value(shot));
			app.exit(ok ? 0 : 1);
		});
	}
	return app.exec();
}
