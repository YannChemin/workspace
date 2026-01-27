#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTreeView>
#include <QLabel>
#include <QFileSystemModel>  // Qt6 replacement for QDirModel
#include <qtermwidget6/qtermwidget.h>
#include <QSplitter>
#include <QSettings>
#include <QCloseEvent>
#include <QShortcut>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QScrollArea>
#include <gdal_priv.h>
#include <cpl_conv.h>   // GDAL utilities

#include <QDir>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onFileSelected(const QModelIndex &index);
    void refreshDisplay();
    void zoomIn();
    void zoomOut();
    void zoomReset();

private:
    void setupUI();
    void loadImage(const QString &path);
    void changeTerminalDir(const QString &path);
    void saveState();
    void restoreState();
    void updateImageDisplay();

    QTreeView *fileTree;
    QScrollArea *scrollArea;
    QLabel *imagePreview;
    QTermWidget *terminal;
    QFileSystemModel *dirModel;  // Qt6 version
    QSplitter *splitter;
    QSplitter *topSplitter;

    QString lastImagePath;
    QPixmap currentPixmap;  // Store original pixmap for proper resizing
    double zoomFactor;      // Current zoom level (1.0 = fit to window)
};

#endif
