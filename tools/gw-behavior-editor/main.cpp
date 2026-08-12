#include "main_window.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QTextStream>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName("GeoWorld 行为工具链编辑器");
    application.setOrganizationName("GeoWorld");
    QFont interface_font;
    interface_font.setFamilies({"WenQuanYi Micro Hei", "Noto Sans CJK SC", "sans-serif"});
    interface_font.setPointSize(10);
    application.setFont(interface_font);
    MainWindow window;
    const auto arguments = application.arguments();
    if (arguments.size() == 3 && arguments.at(1) == "--smoke") {
        QString message;
        const auto valid = window.smokeRoundTrip(arguments.at(2), message);
        QTextStream(valid ? stdout : stderr) << message << '\n';
        return valid ? 0 : 1;
    }
    if (arguments.size() == 2 && !window.loadDocument(arguments.at(1))) {
        QTextStream(stderr) << "无法加载启动文档：" << arguments.at(1) << '\n';
        return 1;
    }
    window.show();
    return application.exec();
}
