#include "mainwindow.h"
#include "gisdisplaywidget.h"

#include <QVBoxLayout>
#include <QFileInfo>
#include <QPixmap>
#include <QDebug>
#include <QApplication>
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    GDALAllRegister();  // Init GDAL

    setWindowTitle("Workspace");
    setupUI();
    restoreState();
    showFullScreen();

    // Ctrl+Q to quit
    QShortcut *quitShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q), this);
    connect(quitShortcut, &QShortcut::activated, this, &QMainWindow::close);

    // F5 to refresh display panel
    QShortcut *refreshShortcut = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(refreshShortcut, &QShortcut::activated, this, &MainWindow::refreshDisplay);

    // Zoom shortcuts
    QShortcut *zoomInShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus), this);
    connect(zoomInShortcut, &QShortcut::activated, this, &MainWindow::zoomIn);
    QShortcut *zoomInShortcut2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal), this);
    connect(zoomInShortcut2, &QShortcut::activated, this, &MainWindow::zoomIn);
    QShortcut *zoomOutShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus), this);
    connect(zoomOutShortcut, &QShortcut::activated, this, &MainWindow::zoomOut);
    QShortcut *zoomResetShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), this);
    connect(zoomResetShortcut, &QShortcut::activated, this, &MainWindow::zoomReset);
}

void MainWindow::setupUI() {
    // File tree (25%) - restricted to user's home directory
    dirModel = new QFileSystemModel(this);
    dirModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    dirModel->setRootPath(QDir::homePath());

    fileTree = new QTreeView(this);
    fileTree->setModel(dirModel);
    fileTree->setRootIndex(dirModel->index(QDir::homePath()));
    fileTree->setHeaderHidden(true);
    fileTree->setAnimated(true);
    fileTree->setIndentation(12);
    fileTree->setItemsExpandable(true);

    // Hide Size, Type, Date Modified columns - show only Name
    fileTree->hideColumn(1);
    fileTree->hideColumn(2);
    fileTree->hideColumn(3);

    // GNOME Adwaita-inspired styling
    fileTree->setStyleSheet(
        "QTreeView {"
        "    background: #242424;"
        "    color: #ffffff;"
        "    border: none;"
        "    outline: none;"
        "    font-size: 13px;"
        "    show-decoration-selected: 1;"
        "}"
        "QTreeView::item {"
        "    padding: 4px 8px;"
        "    border-radius: 6px;"
        "    margin: 1px 4px;"
        "}"
        "QTreeView::item:hover {"
        "    background: rgba(255, 255, 255, 0.08);"
        "}"
        "QTreeView::item:selected {"
        "    background: #3584e4;"
        "    color: #ffffff;"
        "}"
        "QTreeView::item:selected:!active {"
        "    background: #1c71d8;"
        "}"
        "QTreeView::branch {"
        "    background: transparent;"
        "}"
        "QTreeView::branch:has-children:closed {"
        "    border-image: none;"
        "    image: none;"
        "}"
        "QTreeView::branch:has-children:open {"
        "    border-image: none;"
        "    image: none;"
        "}"
    );

    // GIS display widget - replaces scroll area + label
    gisDisplay = new GISDisplayWidget(this);
    gisDisplay->setMinimumSize(400, 300);
    gisDisplay->setStyleSheet("border: 1px solid #555;");

    connect(gisDisplay, &GISDisplayWidget::loadError, this, &MainWindow::onLoadError);

    // Terminal (25%) - minimum height ensures more than one line visible
    terminal = new QTermWidget(this);
    terminal->setShellProgram("/bin/bash");
    terminal->setWorkingDirectory(QDir::homePath());
    terminal->setColorScheme("Linux");  // Black background color scheme
    terminal->setMinimumHeight(100);    // Ensure terminal has reasonable minimum height
    terminal->startShellProgram();

    // Install application-level event filter to intercept Ctrl+Shift+C/V
    qApp->installEventFilter(this);

    // Splitter layout - terminal at bottom
    topSplitter = new QSplitter(Qt::Horizontal, this);
    topSplitter->addWidget(fileTree);
    topSplitter->addWidget(gisDisplay);
    topSplitter->setStretchFactor(0, 1);   // File tree: 33%
    topSplitter->setStretchFactor(1, 2);   // Preview: 67%

    splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(topSplitter);
    splitter->addWidget(terminal);
    splitter->setStretchFactor(0, 11);  // Top area: ~69%
    splitter->setStretchFactor(1, 5);   // Terminal: ~31%

    setCentralWidget(splitter);

    // Connect signals
    connect(fileTree->selectionModel(), &QItemSelectionModel::currentRowChanged,
        this, &MainWindow::onFileSelected);
}

void MainWindow::onFileSelected(const QModelIndex &index) {
    QString path = dirModel->filePath(index);
    QFileInfo info(path);

    if (info.isDir()) {
        changeTerminalDir(path);
        gisDisplay->clear();
    } else if (info.isFile()) {
        changeTerminalDir(info.absolutePath());
        loadImage(path);
    }
}

void MainWindow::loadImage(const QString &path) {
    QFileInfo info(path);
    QString ext = info.suffix().toLower();

    if (ext == "tif" || ext == "tiff") {
        gisDisplay->loadGeoTIFF(path);
        lastImagePath = path;
    } else if (ext == "geojson" || ext == "json") {
        gisDisplay->loadGeoJSON(path);
        lastImagePath = path;
    } else {
        // For non-GIS images, we could add support later
        // For now, try loading as GeoTIFF (GDAL handles many formats)
        gisDisplay->loadGeoTIFF(path);
        lastImagePath = path;
    }
}

void MainWindow::refreshDisplay() {
    if (!lastImagePath.isEmpty() && QFileInfo::exists(lastImagePath)) {
        loadImage(lastImagePath);
    }
}

void MainWindow::zoomIn() {
    gisDisplay->zoomIn();
}

void MainWindow::zoomOut() {
    gisDisplay->zoomOut();
}

void MainWindow::zoomReset() {
    gisDisplay->zoomToFit();
}

void MainWindow::onLoadError(const QString &msg) {
    qDebug() << "Load error:" << msg;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // Intercept Ctrl+Shift+C/V for terminal clipboard operations
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            QWidget *focusWidget = QApplication::focusWidget();
            bool terminalHasFocus = focusWidget && (focusWidget == terminal || terminal->isAncestorOf(focusWidget));

            if (terminalHasFocus) {
                if (keyEvent->key() == Qt::Key_C) {
                    terminal->copyClipboard();
                    return true;
                } else if (keyEvent->key() == Qt::Key_V) {
                    terminal->pasteClipboard();
                    return true;
                }
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeTerminalDir(const QString &path) {
    // Only allow navigation within user's home directory
    if (path.startsWith(QDir::homePath())) {
        terminal->setWorkingDirectory(path);
        terminal->sendText(QString("cd '%1'\n").arg(path));
    }
}

void MainWindow::saveState() {
    QSettings settings;
    settings.setValue("lastFolder", dirModel->filePath(fileTree->currentIndex()));
    settings.setValue("lastImage", lastImagePath);
    settings.setValue("splitterSizes", splitter->saveState());
    settings.setValue("topSplitterSizes", topSplitter->saveState());
}

void MainWindow::restoreState() {
    QSettings settings;
    QString lastFolder = settings.value("lastFolder", QDir::homePath()).toString();
    lastImagePath = settings.value("lastImage").toString();

    // Ensure the folder is within user's home directory
    if (!lastFolder.startsWith(QDir::homePath())) {
        lastFolder = QDir::homePath();
    }

    // Restore the folder in the tree view
    QModelIndex folderIndex = dirModel->index(lastFolder);
    if (folderIndex.isValid()) {
        fileTree->setCurrentIndex(folderIndex);
        fileTree->scrollTo(folderIndex);
    } else {
        fileTree->setCurrentIndex(dirModel->index(QDir::homePath()));
    }

    // Restore the last opened image
    if (!lastImagePath.isEmpty() && QFileInfo::exists(lastImagePath)) {
        loadImage(lastImagePath);
    }

    // Restore splitter sizes
    if (settings.contains("splitterSizes")) {
        splitter->restoreState(settings.value("splitterSizes").toByteArray());
    }
    if (settings.contains("topSplitterSizes")) {
        topSplitter->restoreState(settings.value("topSplitterSizes").toByteArray());
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveState();
    event->accept();
}
