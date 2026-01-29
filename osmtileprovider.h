#ifndef OSMTILEPROVIDER_H
#define OSMTILEPROVIDER_H

#include <QObject>
#include <QImage>
#include <QHash>
#include <QCache>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>

// Key for tile caching: zoom_x_y
struct TileKey {
    int zoom = 0;
    int x = 0;
    int y = 0;

    bool operator==(const TileKey &other) const {
        return zoom == other.zoom && x == other.x && y == other.y;
    }
};

Q_DECLARE_METATYPE(TileKey)

inline uint qHash(const TileKey &key, uint seed = 0) {
    return qHash(key.zoom, seed) ^ qHash(key.x, seed) ^ qHash(key.y, seed);
}

class OSMTileProvider : public QObject {
    Q_OBJECT

public:
    explicit OSMTileProvider(QObject *parent = nullptr);
    ~OSMTileProvider();

    // Request a tile - returns immediately, emits tileReady when available
    // If tile is in cache, returns the cached image and does not emit signal
    QImage requestTile(int x, int y, int zoom);

    // Check if a tile is in cache
    bool hasTile(int x, int y, int zoom) const;

    // Get cached tile (returns null if not cached)
    QImage getCachedTile(int x, int y, int zoom) const;

    // Coordinate conversion utilities
    static int lonToTileX(double lon, int zoom);
    static int latToTileY(double lat, int zoom);
    static double tileXToLon(int x, int zoom);
    static double tileYToLat(int y, int zoom);

    // Calculate appropriate zoom level for a geographic extent
    static int calculateZoom(double minLon, double maxLon,
                            double minLat, double maxLat,
                            int displayWidth, int displayHeight);

    // Create OSM background for given geographic extent
    // Returns composite image from all required tiles (synchronously fetches missing tiles)
    QImage createBackground(double minLon, double maxLon,
                           double minLat, double maxLat,
                           int displayWidth, int displayHeight);

    // Async version - fetches tiles and emits backgroundReady when complete
    void createBackgroundAsync(double minLon, double maxLon,
                              double minLat, double maxLat,
                              int displayWidth, int displayHeight,
                              int requestId);

    // Clear all caches
    void clearCache();

    // Get disk cache directory
    static QString cacheDir();

signals:
    // Emitted when an async tile fetch completes
    void tileReady(int x, int y, int zoom, const QImage &tile);

    // Emitted when a background creation completes
    void backgroundReady(int requestId, const QImage &background);

    // Emitted on network error
    void tileError(int x, int y, int zoom, const QString &error);

private slots:
    void onTileDownloaded();

private:
    // Load tile from disk cache
    QImage loadFromDiskCache(const TileKey &key) const;

    // Save tile to disk cache
    void saveToDiskCache(const TileKey &key, const QImage &tile) const;

    // Fetch tile from network
    void fetchTile(const TileKey &key);

    // Compose tiles into background image
    QImage composeTiles(int minTileX, int maxTileX, int minTileY, int maxTileY, int zoom);

    QNetworkAccessManager *m_networkManager;
    QCache<TileKey, QImage> m_memoryCache;  // In-memory tile cache
    QHash<TileKey, QNetworkReply*> m_pendingRequests;  // Pending network requests
    mutable QMutex m_cacheMutex;

    // Background request tracking
    struct BackgroundRequest {
        int requestId;
        int minTileX, maxTileX, minTileY, maxTileY;
        int zoom;
        int tilesNeeded;
        int tilesReceived;
    };
    QHash<int, BackgroundRequest> m_backgroundRequests;
};

#endif // OSMTILEPROVIDER_H
