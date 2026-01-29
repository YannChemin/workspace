#ifndef GISDISPLAYWIDGET_H
#define GISDISPLAYWIDGET_H

#include <QWidget>
#include <QImage>
#include <gdal_priv.h>

#include "gisdatatypes.h"

class OSMTileProvider;

class GISDisplayWidget : public QWidget {
    Q_OBJECT

public:
    explicit GISDisplayWidget(QWidget *parent = nullptr);
    ~GISDisplayWidget();

    // Load data sources
    void loadGeoTIFF(const QString &path);
    void loadGeoJSON(const QString &path);
    void clear();

    // Zoom control
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    double zoomLevel() const { return m_zoomLevel; }

    // Get current file path
    QString currentPath() const { return m_currentPath; }

signals:
    void zoomChanged(double zoom);
    void loadError(const QString &msg);
    void loadSuccess(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onOSMBackgroundReady(int requestId, const QImage &background);
    void onOSMTileReady(int x, int y, int zoom, const QImage &tile);

private:
    // Rendering methods
    void renderViewport();
    QImage renderRasterLOD(const QRect &sourcePixels, const QSize &outputSize);
    void renderGeoJSON();
    void requestOSMTiles();
    void drawOSMTiles(QPainter &painter, double visMinLon, double visMaxLon,
                      double visMinLat, double visMaxLat);
    void drawDataCentered(QPainter &painter);

    // Helper methods
    void calculateVisibleBounds();
    QRect geoToPixelRect(double minLon, double maxLon, double minLat, double maxLat) const;
    void applyHistogramStretch(std::vector<float> &data, GDALRasterBand *band,
                               std::vector<uint8_t> &output);

    // RGB band selection helpers
    RGBBands selectRGBBands(GDALDataset *dataset);
    std::vector<double> parseWavelengths(GDALDataset *dataset);
    int findClosestBand(const std::vector<double> &wavelengths, double targetNm);

    // Statistics helpers
    BandStatistics getCachedStatistics(GDALRasterBand *band);
    BandStatistics computeAndCacheStatistics(GDALRasterBand *band);
    StretchParams stretchParamsFromStatistics(const BandStatistics &stats);
    StretchParams calculateStretchParams(std::vector<float> &data, double lowPct, double highPct);
    uint8_t stretchValue(float val, double minP, double maxP);
    NoDataInfo getNoDataValue(GDALRasterBand *band);
    bool isNoData(float pixelValue, const NoDataInfo &nodata);

    // Coordinate transformation
    GeoTIFFInfo getGeoTIFFBounds();
    GeoJSONInfo getGeoJSONBounds(const QString &path);

    // Data handles (kept open for LOD rendering)
    GDALDataset *m_rasterDataset = nullptr;
    GDALDataset *m_vectorDataset = nullptr;
    QString m_currentPath;

    // View state
    double m_zoomLevel = 1.0;        // 1.0 = fit to widget
    QPointF m_centerGeo;              // Center of view in geographic coordinates
    QPointF m_lastMousePos;
    bool m_isPanning = false;
    bool m_fitToWidget = true;        // True = auto-fit, false = manual zoom

    // Data properties
    GeoTIFFInfo m_geoInfo;
    GeoJSONInfo m_geoJSONInfo;
    RGBBands m_bands;
    GeoTransform m_geoTransform;
    bool m_hasRaster = false;
    bool m_hasVector = false;

    // Cached rendered images
    QImage m_rasterImage;             // Current LOD-rendered raster
    QImage m_vectorImage;             // Rendered vector overlay
    QImage m_osmBackground;           // OSM tile background
    QRect m_renderedSourceRect;       // Source rect that m_rasterImage covers
    double m_renderedZoom = 0.0;      // Zoom level at which cache was rendered

    // OSM tile provider
    OSMTileProvider *m_osmProvider;
    int m_osmZoom = 0;  // Current OSM tile zoom level
};

#endif // GISDISPLAYWIDGET_H
