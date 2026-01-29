#include "osmtileprovider.h"
#include "gisdatatypes.h"

#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QDebug>
#include <QPainter>
#include <QEventLoop>
#include <cmath>

OSMTileProvider::OSMTileProvider(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_memoryCache(100)  // Cache up to 100 tiles in memory
{
    // Register TileKey for use with QVariant
    qRegisterMetaType<TileKey>("TileKey");
}

OSMTileProvider::~OSMTileProvider()
{
    // Cancel pending requests
    for (auto *reply : m_pendingRequests.values()) {
        reply->abort();
        reply->deleteLater();
    }
}

QString OSMTileProvider::cacheDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/osm_tiles";
    QDir().mkpath(dir);
    return dir;
}

int OSMTileProvider::lonToTileX(double lon, int zoom)
{
    return static_cast<int>(std::floor((lon + 180.0) / 360.0 * (1 << zoom)));
}

int OSMTileProvider::latToTileY(double lat, int zoom)
{
    double latRad = lat * M_PI / 180.0;
    return static_cast<int>(std::floor((1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * (1 << zoom)));
}

double OSMTileProvider::tileXToLon(int x, int zoom)
{
    return x / static_cast<double>(1 << zoom) * 360.0 - 180.0;
}

double OSMTileProvider::tileYToLat(int y, int zoom)
{
    double n = M_PI - 2.0 * M_PI * y / static_cast<double>(1 << zoom);
    return 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

int OSMTileProvider::calculateZoom(double minLon, double maxLon,
                                   double minLat, double maxLat,
                                   int displayWidth, int displayHeight)
{
    for (int zoom = 18; zoom >= 0; --zoom) {
        int minTileX = lonToTileX(minLon, zoom);
        int maxTileX = lonToTileX(maxLon, zoom);
        int minTileY = latToTileY(maxLat, zoom);  // Note: Y is inverted
        int maxTileY = latToTileY(minLat, zoom);

        int tilesX = maxTileX - minTileX + 1;
        int tilesY = maxTileY - minTileY + 1;

        // Check if this zoom level gives reasonable coverage
        if (tilesX * OSM_TILE_SIZE <= displayWidth * 2 &&
            tilesY * OSM_TILE_SIZE <= displayHeight * 2) {
            return zoom;
        }
    }
    return 4;  // Default to low zoom
}

bool OSMTileProvider::hasTile(int x, int y, int zoom) const
{
    TileKey key{zoom, x, y};
    QMutexLocker lock(&m_cacheMutex);
    return m_memoryCache.contains(key);
}

QImage OSMTileProvider::getCachedTile(int x, int y, int zoom) const
{
    TileKey key{zoom, x, y};
    QMutexLocker lock(&m_cacheMutex);
    QImage *cached = m_memoryCache.object(key);
    if (cached) {
        return *cached;
    }
    return QImage();
}

QImage OSMTileProvider::loadFromDiskCache(const TileKey &key) const
{
    QString path = QString("%1/%2_%3_%4.png")
                       .arg(cacheDir())
                       .arg(key.zoom)
                       .arg(key.x)
                       .arg(key.y);

    if (QFileInfo::exists(path)) {
        QImage tile(path);
        if (!tile.isNull()) {
            return tile;
        }
    }
    return QImage();
}

void OSMTileProvider::saveToDiskCache(const TileKey &key, const QImage &tile) const
{
    QString path = QString("%1/%2_%3_%4.png")
                       .arg(cacheDir())
                       .arg(key.zoom)
                       .arg(key.x)
                       .arg(key.y);
    tile.save(path, "PNG");
}

QImage OSMTileProvider::requestTile(int x, int y, int zoom)
{
    TileKey key{zoom, x, y};

    // Check memory cache
    {
        QMutexLocker lock(&m_cacheMutex);
        QImage *cached = m_memoryCache.object(key);
        if (cached) {
            return *cached;
        }
    }

    // Check disk cache
    QImage diskCached = loadFromDiskCache(key);
    if (!diskCached.isNull()) {
        QMutexLocker lock(&m_cacheMutex);
        m_memoryCache.insert(key, new QImage(diskCached));
        return diskCached;
    }

    // Need to fetch from network
    fetchTile(key);
    return QImage();  // Return empty, will emit tileReady when downloaded
}

void OSMTileProvider::fetchTile(const TileKey &key)
{
    // Check if already fetching
    if (m_pendingRequests.contains(key)) {
        return;
    }

    QString url = QString("https://tile.openstreetmap.org/%1/%2/%3.png")
                      .arg(key.zoom)
                      .arg(key.x)
                      .arg(key.y);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "WorkspaceApp/1.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                        QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_networkManager->get(request);
    reply->setProperty("tileKey", QVariant::fromValue(key));
    m_pendingRequests.insert(key, reply);

    connect(reply, &QNetworkReply::finished, this, &OSMTileProvider::onTileDownloaded);
}

void OSMTileProvider::onTileDownloaded()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    TileKey key = reply->property("tileKey").value<TileKey>();
    m_pendingRequests.remove(key);

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QImage tile;
        if (tile.loadFromData(data)) {
            // Cache in memory
            {
                QMutexLocker lock(&m_cacheMutex);
                m_memoryCache.insert(key, new QImage(tile));
            }

            // Cache to disk
            saveToDiskCache(key, tile);

            emit tileReady(key.x, key.y, key.zoom, tile);

            // Check if any background request needs this tile
            for (auto it = m_backgroundRequests.begin(); it != m_backgroundRequests.end();) {
                BackgroundRequest &req = *it;
                if (key.zoom == req.zoom &&
                    key.x >= req.minTileX && key.x <= req.maxTileX &&
                    key.y >= req.minTileY && key.y <= req.maxTileY) {
                    req.tilesReceived++;
                    if (req.tilesReceived >= req.tilesNeeded) {
                        // All tiles ready, compose background
                        QImage bg = composeTiles(req.minTileX, req.maxTileX,
                                                req.minTileY, req.maxTileY, req.zoom);
                        int reqId = req.requestId;
                        it = m_backgroundRequests.erase(it);
                        emit backgroundReady(reqId, bg);
                        continue;
                    }
                }
                ++it;
            }
        } else {
            emit tileError(key.x, key.y, key.zoom, "Failed to decode tile image");
        }
    } else {
        emit tileError(key.x, key.y, key.zoom, reply->errorString());
    }

    reply->deleteLater();
}

QImage OSMTileProvider::composeTiles(int minTileX, int maxTileX,
                                     int minTileY, int maxTileY, int zoom)
{
    int tilesX = maxTileX - minTileX + 1;
    int tilesY = maxTileY - minTileY + 1;

    QImage composite(tilesX * OSM_TILE_SIZE, tilesY * OSM_TILE_SIZE, QImage::Format_ARGB32);
    composite.fill(QColor(30, 30, 30));

    QPainter painter(&composite);
    for (int ty = minTileY; ty <= maxTileY; ++ty) {
        for (int tx = minTileX; tx <= maxTileX; ++tx) {
            QImage tile = getCachedTile(tx, ty, zoom);
            if (tile.isNull()) {
                tile = loadFromDiskCache({zoom, tx, ty});
            }
            if (!tile.isNull()) {
                int px = (tx - minTileX) * OSM_TILE_SIZE;
                int py = (ty - minTileY) * OSM_TILE_SIZE;
                painter.drawImage(px, py, tile);
            }
        }
    }
    painter.end();

    // Store extent info as metadata
    composite.setText("minLon", QString::number(tileXToLon(minTileX, zoom), 'f', 10));
    composite.setText("maxLon", QString::number(tileXToLon(maxTileX + 1, zoom), 'f', 10));
    composite.setText("minLat", QString::number(tileYToLat(maxTileY + 1, zoom), 'f', 10));
    composite.setText("maxLat", QString::number(tileYToLat(minTileY, zoom), 'f', 10));

    return composite;
}

QImage OSMTileProvider::createBackground(double minLon, double maxLon,
                                         double minLat, double maxLat,
                                         int displayWidth, int displayHeight)
{
    int zoom = calculateZoom(minLon, maxLon, minLat, maxLat, displayWidth, displayHeight);

    // Get tile range
    int minTileX = lonToTileX(minLon, zoom);
    int maxTileX = lonToTileX(maxLon, zoom);
    int minTileY = latToTileY(maxLat, zoom);
    int maxTileY = latToTileY(minLat, zoom);

    // Add buffer tiles
    int maxTileIdx = (1 << zoom) - 1;
    minTileX = std::max(0, minTileX - 1);
    maxTileX = std::min(maxTileIdx, maxTileX + 1);
    minTileY = std::max(0, minTileY - 1);
    maxTileY = std::min(maxTileIdx, maxTileY + 1);

    qDebug() << "Creating OSM background: zoom" << zoom
             << "tiles" << (maxTileX - minTileX + 1) << "x" << (maxTileY - minTileY + 1);

    // Fetch all needed tiles synchronously
    for (int ty = minTileY; ty <= maxTileY; ++ty) {
        for (int tx = minTileX; tx <= maxTileX; ++tx) {
            QImage tile = requestTile(tx, ty, zoom);
            if (tile.isNull()) {
                // Need to wait for network fetch
                TileKey key{zoom, tx, ty};
                if (m_pendingRequests.contains(key)) {
                    QNetworkReply *reply = m_pendingRequests.value(key);
                    QEventLoop loop;
                    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
                    loop.exec();
                }
            }
        }
    }

    return composeTiles(minTileX, maxTileX, minTileY, maxTileY, zoom);
}

void OSMTileProvider::createBackgroundAsync(double minLon, double maxLon,
                                           double minLat, double maxLat,
                                           int displayWidth, int displayHeight,
                                           int requestId)
{
    int zoom = calculateZoom(minLon, maxLon, minLat, maxLat, displayWidth, displayHeight);

    // Get tile range
    int minTileX = lonToTileX(minLon, zoom);
    int maxTileX = lonToTileX(maxLon, zoom);
    int minTileY = latToTileY(maxLat, zoom);
    int maxTileY = latToTileY(minLat, zoom);

    // Add buffer tiles
    int maxTileIdx = (1 << zoom) - 1;
    minTileX = std::max(0, minTileX - 1);
    maxTileX = std::min(maxTileIdx, maxTileX + 1);
    minTileY = std::max(0, minTileY - 1);
    maxTileY = std::min(maxTileIdx, maxTileY + 1);

    int tilesNeeded = (maxTileX - minTileX + 1) * (maxTileY - minTileY + 1);
    int tilesReady = 0;

    // Request all tiles
    for (int ty = minTileY; ty <= maxTileY; ++ty) {
        for (int tx = minTileX; tx <= maxTileX; ++tx) {
            QImage tile = requestTile(tx, ty, zoom);
            if (!tile.isNull()) {
                tilesReady++;
            }
        }
    }

    if (tilesReady == tilesNeeded) {
        // All tiles already cached, emit immediately
        QImage bg = composeTiles(minTileX, maxTileX, minTileY, maxTileY, zoom);
        emit backgroundReady(requestId, bg);
    } else {
        // Track this request
        BackgroundRequest req;
        req.requestId = requestId;
        req.minTileX = minTileX;
        req.maxTileX = maxTileX;
        req.minTileY = minTileY;
        req.maxTileY = maxTileY;
        req.zoom = zoom;
        req.tilesNeeded = tilesNeeded;
        req.tilesReceived = tilesReady;
        m_backgroundRequests.insert(requestId, req);
    }
}

void OSMTileProvider::clearCache()
{
    QMutexLocker lock(&m_cacheMutex);
    m_memoryCache.clear();
}
