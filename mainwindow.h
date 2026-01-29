#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTreeView>
#include <QFileSystemModel>
#include <qtermwidget6/qtermwidget.h>
#include <QSplitter>
#include <QSettings>
#include <QCloseEvent>
#include <QShortcut>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QDir>
#include <gdal_priv.h>

class GISDisplayWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onFileSelected(const QModelIndex &index);
    void refreshDisplay();
    void zoomIn();
    void zoomOut();
    void zoomReset();
    void onLoadError(const QString &msg);

private:
    void setupUI();
    void loadImage(const QString &path);
    void changeTerminalDir(const QString &path);
    void saveState();
    void restoreState();

    QTreeView *fileTree;
    GISDisplayWidget *gisDisplay;
    QTermWidget *terminal;
    QFileSystemModel *dirModel;
    QSplitter *splitter;
    QSplitter *topSplitter;

    QString lastImagePath;
};

#endif
