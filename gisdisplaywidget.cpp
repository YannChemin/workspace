#include "gisdisplaywidget.h"
#include "osmtileprovider.h"

#include <QPainter>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QDebug>
#include <QRegularExpression>
#include <QFileInfo>

#include <ogrsf_frmts.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <limits>

GISDisplayWidget::GISDisplayWidget(QWidget *parent)
    : QWidget(parent)
    , m_osmProvider(new OSMTileProvider(this))
{
    setMinimumSize(400, 300);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Connect tile ready signal for incremental updates
    connect(m_osmProvider, &OSMTileProvider::tileReady,
            this, &GISDisplayWidget::onOSMTileReady);
}

GISDisplayWidget::~GISDisplayWidget()
{
    clear();
}

void GISDisplayWidget::clear()
{
    if (m_rasterDataset) {
        GDALClose(m_rasterDataset);
        m_rasterDataset = nullptr;
    }
    if (m_vectorDataset) {
        GDALClose(m_vectorDataset);
        m_vectorDataset = nullptr;
    }

    m_currentPath.clear();
    m_hasRaster = false;
    m_hasVector = false;
    m_rasterImage = QImage();
    m_vectorImage = QImage();
    m_fitToWidget = true;
    m_zoomLevel = 1.0;
    m_osmZoom = 0;

    update();
}

void GISDisplayWidget::loadGeoTIFF(const QString &path)
{
    if (m_rasterDataset) {
        GDALClose(m_rasterDataset);
        m_rasterDataset = nullptr;
    }

    m_rasterDataset = static_cast<GDALDataset*>(GDALOpen(path.toStdString().c_str(), GA_ReadOnly));
    if (!m_rasterDataset) {
        emit loadError("Failed to open GeoTIFF: " + path);
        return;
    }

    m_currentPath = path;
    m_hasRaster = true;
    m_geoInfo = getGeoTIFFBounds();
    m_bands = selectRGBBands(m_rasterDataset);

    double gt[6];
    if (m_rasterDataset->GetGeoTransform(gt) == CE_None) {
        m_geoTransform.set(gt);
    }

    if (m_geoInfo.hasValidCRS) {
        m_centerGeo = QPointF((m_geoInfo.minLon + m_geoInfo.maxLon) / 2.0,
                              (m_geoInfo.minLat + m_geoInfo.maxLat) / 2.0);
    }

    m_fitToWidget = true;
    m_zoomLevel = 1.0;
    m_rasterImage = QImage();
    m_renderedZoom = 0.0;

    renderViewport();
    requestOSMTiles();
    update();

    emit loadSuccess(path);
}

void GISDisplayWidget::loadGeoJSON(const QString &path)
{
    if (m_vectorDataset) {
        GDALClose(m_vectorDataset);
        m_vectorDataset = nullptr;
    }

    m_vectorDataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.toStdString().c_str(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr
    ));

    if (!m_vectorDataset) {
        emit loadError("Failed to open GeoJSON: " + path);
        return;
    }

    m_currentPath = path;
    m_hasVector = true;
    m_geoJSONInfo = getGeoJSONBounds(path);

    if (m_geoJSONInfo.hasValidBounds) {
        m_centerGeo = QPointF((m_geoJSONInfo.minLon + m_geoJSONInfo.maxLon) / 2.0,
                              (m_geoJSONInfo.minLat + m_geoJSONInfo.maxLat) / 2.0);
    }

    m_fitToWidget = true;
    m_zoomLevel = 1.0;
    m_vectorImage = QImage();

    renderGeoJSON();
    requestOSMTiles();
    update();

    emit loadSuccess(path);
}

void GISDisplayWidget::zoomIn()
{
    if (!m_fitToWidget) {
        m_zoomLevel *= 1.25;
    } else {
        m_fitToWidget = false;
        m_zoomLevel = 1.25;
    }
    if (m_zoomLevel > 10.0) m_zoomLevel = 10.0;

    renderViewport();
    renderGeoJSON();
    requestOSMTiles();
    update();
    emit zoomChanged(m_zoomLevel);
}

void GISDisplayWidget::zoomOut()
{
    if (!m_fitToWidget) {
        m_zoomLevel /= 1.25;
        if (m_zoomLevel < 1.0) {
            m_fitToWidget = true;
            m_zoomLevel = 1.0;
        }
    }

    renderViewport();
    renderGeoJSON();
    requestOSMTiles();
    update();
    emit zoomChanged(m_zoomLevel);
}

void GISDisplayWidget::zoomToFit()
{
    m_fitToWidget = true;
    m_zoomLevel = 1.0;
    m_rasterImage = QImage();
    m_vectorImage = QImage();
    m_renderedZoom = 0.0;

    renderViewport();
    renderGeoJSON();
    requestOSMTiles();
    update();
    emit zoomChanged(m_zoomLevel);
}

void GISDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(30, 30, 30));

    if (!m_hasRaster && !m_hasVector) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, "Select a GIS file...");
        return;
    }

    // Get data bounds
    double dataMinLon, dataMaxLon, dataMinLat, dataMaxLat;
    if (m_hasRaster && m_geoInfo.hasValidCRS) {
        dataMinLon = m_geoInfo.minLon;
        dataMaxLon = m_geoInfo.maxLon;
        dataMinLat = m_geoInfo.minLat;
        dataMaxLat = m_geoInfo.maxLat;
    } else if (m_hasVector && m_geoJSONInfo.hasValidBounds) {
        dataMinLon = m_geoJSONInfo.minLon;
        dataMaxLon = m_geoJSONInfo.maxLon;
        dataMinLat = m_geoJSONInfo.minLat;
        dataMaxLat = m_geoJSONInfo.maxLat;
    } else {
        // No geo bounds, just draw data centered
        drawDataCentered(painter);
        return;
    }

    // Calculate visible geographic extent based on zoom
    double dataLonRange = dataMaxLon - dataMinLon;
    double dataLatRange = dataMaxLat - dataMinLat;

    double visLonRange, visLatRange;
    if (m_fitToWidget) {
        visLonRange = dataLonRange * 1.4;  // Add buffer
        visLatRange = dataLatRange * 1.4;
    } else {
        visLonRange = dataLonRange / m_zoomLevel * 1.2;
        visLatRange = dataLatRange / m_zoomLevel * 1.2;
    }

    // Adjust for widget aspect ratio
    double widgetAspect = static_cast<double>(width()) / std::max(1, height());
    double visAspect = visLonRange / std::max(visLatRange, 0.0001);

    if (visAspect > widgetAspect) {
        visLatRange = visLonRange / widgetAspect;
    } else {
        visLonRange = visLatRange * widgetAspect;
    }

    double visMinLon = m_centerGeo.x() - visLonRange / 2.0;
    double visMaxLon = m_centerGeo.x() + visLonRange / 2.0;
    double visMinLat = m_centerGeo.y() - visLatRange / 2.0;
    double visMaxLat = m_centerGeo.y() + visLatRange / 2.0;

    // Draw OSM tiles as background (best effort - draw what's available)
    if (m_osmZoom > 0) {
        drawOSMTiles(painter, visMinLon, visMaxLon, visMinLat, visMaxLat);
    }

    // Calculate pixel coordinates for data
    auto geoToPixel = [&](double lon, double lat) -> QPointF {
        double x = (lon - visMinLon) / visLonRange * width();
        double y = (visMaxLat - lat) / visLatRange * height();
        return QPointF(x, y);
    };

    // Draw data on top
    QPointF topLeft = geoToPixel(dataMinLon, dataMaxLat);
    QPointF bottomRight = geoToPixel(dataMaxLon, dataMinLat);
    QRectF dataRect(topLeft, bottomRight);

    if (m_hasRaster && !m_rasterImage.isNull()) {
        painter.drawImage(dataRect, m_rasterImage);
    }

    if (m_hasVector && !m_vectorImage.isNull()) {
        painter.drawImage(dataRect, m_vectorImage);
    }
}

void GISDisplayWidget::drawDataCentered(QPainter &painter)
{
    // Fallback: just draw data centered without geo-alignment
    if (m_hasRaster && !m_rasterImage.isNull()) {
        QSize scaled = m_rasterImage.size().scaled(size(), Qt::KeepAspectRatio);
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        painter.drawImage(QRect(x, y, scaled.width(), scaled.height()), m_rasterImage);
    }
    if (m_hasVector && !m_vectorImage.isNull()) {
        QSize scaled = m_vectorImage.size().scaled(size(), Qt::KeepAspectRatio);
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        painter.drawImage(QRect(x, y, scaled.width(), scaled.height()), m_vectorImage);
    }
}

void GISDisplayWidget::drawOSMTiles(QPainter &painter, double visMinLon, double visMaxLon,
                                    double visMinLat, double visMaxLat)
{
    if (m_osmZoom <= 0) return;

    double visLonRange = visMaxLon - visMinLon;
    double visLatRange = visMaxLat - visMinLat;
    if (visLonRange <= 0 || visLatRange <= 0) return;

    // Get tile range for visible area
    int minTileX = OSMTileProvider::lonToTileX(visMinLon, m_osmZoom);
    int maxTileX = OSMTileProvider::lonToTileX(visMaxLon, m_osmZoom);
    int minTileY = OSMTileProvider::latToTileY(visMaxLat, m_osmZoom);
    int maxTileY = OSMTileProvider::latToTileY(visMinLat, m_osmZoom);

    // Clamp to valid range
    int maxIdx = (1 << m_osmZoom) - 1;
    minTileX = std::max(0, minTileX);
    maxTileX = std::min(maxIdx, maxTileX);
    minTileY = std::max(0, minTileY);
    maxTileY = std::min(maxIdx, maxTileY);

    // Draw each tile that's available
    for (int ty = minTileY; ty <= maxTileY; ++ty) {
        for (int tx = minTileX; tx <= maxTileX; ++tx) {
            QImage tile = m_osmProvider->getCachedTile(tx, ty, m_osmZoom);
            if (tile.isNull()) {
                // Request tile if not cached
                m_osmProvider->requestTile(tx, ty, m_osmZoom);
                continue;
            }

            // Calculate tile bounds in geo coordinates
            double tileLonMin = OSMTileProvider::tileXToLon(tx, m_osmZoom);
            double tileLonMax = OSMTileProvider::tileXToLon(tx + 1, m_osmZoom);
            double tileLatMax = OSMTileProvider::tileYToLat(ty, m_osmZoom);
            double tileLatMin = OSMTileProvider::tileYToLat(ty + 1, m_osmZoom);

            // Convert to pixel coordinates
            double x1 = (tileLonMin - visMinLon) / visLonRange * width();
            double x2 = (tileLonMax - visMinLon) / visLonRange * width();
            double y1 = (visMaxLat - tileLatMax) / visLatRange * height();
            double y2 = (visMaxLat - tileLatMin) / visLatRange * height();

            QRectF destRect(x1, y1, x2 - x1, y2 - y1);
            painter.drawImage(destRect, tile);
        }
    }
}

void GISDisplayWidget::wheelEvent(QWheelEvent *event)
{
    if (!m_hasRaster && !m_hasVector) {
        event->ignore();
        return;
    }

    if (m_fitToWidget) {
        m_fitToWidget = false;
        m_zoomLevel = 1.0;
    }

    double factor = (event->angleDelta().y() > 0) ? 1.15 : 1.0 / 1.15;
    m_zoomLevel *= factor;
    m_zoomLevel = qBound(0.5, m_zoomLevel, 20.0);

    if (m_zoomLevel < 1.0) {
        m_fitToWidget = true;
        m_zoomLevel = 1.0;
    }

    renderViewport();
    renderGeoJSON();
    requestOSMTiles();
    update();
    emit zoomChanged(m_zoomLevel);
    event->accept();
}

void GISDisplayWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_isPanning = true;
        m_lastMousePos = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }
}

void GISDisplayWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_isPanning && !m_fitToWidget) {
        QPointF delta = event->position() - m_lastMousePos;
        m_lastMousePos = event->position();

        // Get data bounds for scale calculation
        double dataLonRange, dataLatRange;
        if (m_hasRaster && m_geoInfo.hasValidCRS) {
            dataLonRange = m_geoInfo.maxLon - m_geoInfo.minLon;
            dataLatRange = m_geoInfo.maxLat - m_geoInfo.minLat;
        } else if (m_hasVector && m_geoJSONInfo.hasValidBounds) {
            dataLonRange = m_geoJSONInfo.maxLon - m_geoJSONInfo.minLon;
            dataLatRange = m_geoJSONInfo.maxLat - m_geoJSONInfo.minLat;
        } else {
            return;
        }

        double visLonRange = dataLonRange / m_zoomLevel;
        double visLatRange = dataLatRange / m_zoomLevel;

        double geoPerPixelX = visLonRange / width();
        double geoPerPixelY = visLatRange / height();

        m_centerGeo.rx() -= delta.x() * geoPerPixelX;
        m_centerGeo.ry() += delta.y() * geoPerPixelY;

        requestOSMTiles();
        update();
        event->accept();
    }
}

void GISDisplayWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_isPanning = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
    }
}

void GISDisplayWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_fitToWidget) {
        m_rasterImage = QImage();
        m_renderedZoom = 0.0;
    }
    renderViewport();
    renderGeoJSON();
    requestOSMTiles();
    update();
}

void GISDisplayWidget::onOSMTileReady(int, int, int zoom, const QImage &)
{
    // Only repaint if the tile is for our current zoom level
    if (zoom == m_osmZoom) {
        update();
    }
}

void GISDisplayWidget::onOSMBackgroundReady(int, const QImage &)
{
    // Not used in new architecture
}

void GISDisplayWidget::requestOSMTiles()
{
    // Calculate appropriate OSM zoom level based on current view
    double dataLonRange, dataLatRange;
    if (m_hasRaster && m_geoInfo.hasValidCRS) {
        dataLonRange = m_geoInfo.maxLon - m_geoInfo.minLon;
        dataLatRange = m_geoInfo.maxLat - m_geoInfo.minLat;
    } else if (m_hasVector && m_geoJSONInfo.hasValidBounds) {
        dataLonRange = m_geoJSONInfo.maxLon - m_geoJSONInfo.minLon;
        dataLatRange = m_geoJSONInfo.maxLat - m_geoJSONInfo.minLat;
    } else {
        m_osmZoom = 0;
        return;
    }

    double visLonRange = m_fitToWidget ? dataLonRange * 1.4 : dataLonRange / m_zoomLevel * 1.2;
    double visLatRange = m_fitToWidget ? dataLatRange * 1.4 : dataLatRange / m_zoomLevel * 1.2;

    double visMinLon = m_centerGeo.x() - visLonRange / 2.0;
    double visMaxLon = m_centerGeo.x() + visLonRange / 2.0;
    double visMinLat = m_centerGeo.y() - visLatRange / 2.0;
    double visMaxLat = m_centerGeo.y() + visLatRange / 2.0;

    // Calculate zoom level
    m_osmZoom = OSMTileProvider::calculateZoom(visMinLon, visMaxLon, visMinLat, visMaxLat,
                                                width(), height());

    // Request tiles for visible area
    int minTileX = OSMTileProvider::lonToTileX(visMinLon, m_osmZoom);
    int maxTileX = OSMTileProvider::lonToTileX(visMaxLon, m_osmZoom);
    int minTileY = OSMTileProvider::latToTileY(visMaxLat, m_osmZoom);
    int maxTileY = OSMTileProvider::latToTileY(visMinLat, m_osmZoom);

    int maxIdx = (1 << m_osmZoom) - 1;
    minTileX = std::max(0, minTileX);
    maxTileX = std::min(maxIdx, maxTileX);
    minTileY = std::max(0, minTileY);
    maxTileY = std::min(maxIdx, maxTileY);

    // Pre-fetch tiles
    for (int ty = minTileY; ty <= maxTileY; ++ty) {
        for (int tx = minTileX; tx <= maxTileX; ++tx) {
            m_osmProvider->requestTile(tx, ty, m_osmZoom);
        }
    }
}

void GISDisplayWidget::renderViewport()
{
    if (!m_hasRaster || !m_rasterDataset) {
        return;
    }

    int rasterW = m_rasterDataset->GetRasterXSize();
    int rasterH = m_rasterDataset->GetRasterYSize();

    // Render at a reasonable size for display
    QSize outputSize = QSize(rasterW, rasterH).scaled(size() * 2, Qt::KeepAspectRatio);
    if (outputSize.width() > 2048) outputSize.setWidth(2048);
    if (outputSize.height() > 2048) outputSize.setHeight(2048);

    QRect sourceRect(0, 0, rasterW, rasterH);

    if (m_renderedZoom != m_zoomLevel || m_rasterImage.isNull()) {
        m_rasterImage = renderRasterLOD(sourceRect, outputSize);
        m_renderedZoom = m_zoomLevel;
        m_renderedSourceRect = sourceRect;
    }
}

QImage GISDisplayWidget::renderRasterLOD(const QRect &sourcePixels, const QSize &outputSize)
{
    if (!m_rasterDataset) {
        return QImage();
    }

    int bandCount = m_rasterDataset->GetRasterCount();
    int outW = outputSize.width();
    int outH = outputSize.height();

    QImage image(outW, outH, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    if (bandCount >= 3) {
        std::vector<float> r(outW * outH), g(outW * outH), b(outW * outH);

        GDALRasterBand *bandR = m_rasterDataset->GetRasterBand(m_bands.red);
        GDALRasterBand *bandG = m_rasterDataset->GetRasterBand(m_bands.green);
        GDALRasterBand *bandB = m_rasterDataset->GetRasterBand(m_bands.blue);

        CPLErr errR = bandR->RasterIO(GF_Read,
            sourcePixels.x(), sourcePixels.y(),
            sourcePixels.width(), sourcePixels.height(),
            r.data(), outW, outH, GDT_Float32, 0, 0);
        CPLErr errG = bandG->RasterIO(GF_Read,
            sourcePixels.x(), sourcePixels.y(),
            sourcePixels.width(), sourcePixels.height(),
            g.data(), outW, outH, GDT_Float32, 0, 0);
        CPLErr errB = bandB->RasterIO(GF_Read,
            sourcePixels.x(), sourcePixels.y(),
            sourcePixels.width(), sourcePixels.height(),
            b.data(), outW, outH, GDT_Float32, 0, 0);

        if (errR != CE_None || errG != CE_None || errB != CE_None) {
            return image;
        }

        NoDataInfo nodataR = getNoDataValue(bandR);
        NoDataInfo nodataG = getNoDataValue(bandG);
        NoDataInfo nodataB = getNoDataValue(bandB);

        BandStatistics statsR = getCachedStatistics(bandR);
        BandStatistics statsG = getCachedStatistics(bandG);
        BandStatistics statsB = getCachedStatistics(bandB);

        if (!statsR.valid) statsR = computeAndCacheStatistics(bandR);
        if (!statsG.valid) statsG = computeAndCacheStatistics(bandG);
        if (!statsB.valid) statsB = computeAndCacheStatistics(bandB);

        StretchParams paramsR = stretchParamsFromStatistics(statsR);
        StretchParams paramsG = stretchParamsFromStatistics(statsG);
        StretchParams paramsB = stretchParamsFromStatistics(statsB);

        if (!paramsR.valid) paramsR = calculateStretchParams(r, 0.02, 0.98);
        if (!paramsG.valid) paramsG = calculateStretchParams(g, 0.02, 0.98);
        if (!paramsB.valid) paramsB = calculateStretchParams(b, 0.02, 0.98);

        for (int y = 0; y < outH; ++y) {
            QRgb *scanline = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < outW; ++x) {
                size_t idx = static_cast<size_t>(y) * outW + x;

                if (isNoData(r[idx], nodataR) || isNoData(g[idx], nodataG) || isNoData(b[idx], nodataB)) {
                    scanline[x] = qRgba(0, 0, 0, 0);
                    continue;
                }

                uint8_t rv = stretchValue(r[idx], paramsR.minP, paramsR.maxP);
                uint8_t gv = stretchValue(g[idx], paramsG.minP, paramsG.maxP);
                uint8_t bv = stretchValue(b[idx], paramsB.minP, paramsB.maxP);
                scanline[x] = qRgba(rv, gv, bv, 255);
            }
        }
    } else if (bandCount == 1) {
        std::vector<float> data(outW * outH);
        GDALRasterBand *band = m_rasterDataset->GetRasterBand(1);

        CPLErr err = band->RasterIO(GF_Read,
            sourcePixels.x(), sourcePixels.y(),
            sourcePixels.width(), sourcePixels.height(),
            data.data(), outW, outH, GDT_Float32, 0, 0);

        if (err != CE_None) {
            return image;
        }

        NoDataInfo nodata = getNoDataValue(band);
        BandStatistics stats = getCachedStatistics(band);
        if (!stats.valid) stats = computeAndCacheStatistics(band);
        StretchParams params = stretchParamsFromStatistics(stats);
        if (!params.valid) params = calculateStretchParams(data, 0.02, 0.98);

        for (int y = 0; y < outH; ++y) {
            QRgb *scanline = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < outW; ++x) {
                size_t idx = static_cast<size_t>(y) * outW + x;

                if (isNoData(data[idx], nodata)) {
                    scanline[x] = qRgba(0, 0, 0, 0);
                    continue;
                }

                uint8_t gray = stretchValue(data[idx], params.minP, params.maxP);
                scanline[x] = qRgba(gray, gray, gray, 255);
            }
        }
    }

    return image;
}

void GISDisplayWidget::renderGeoJSON()
{
    if (!m_vectorDataset) {
        return;
    }

    OGRLayer *layer = m_vectorDataset->GetLayer(0);
    if (!layer) {
        return;
    }

    OGREnvelope extent;
    if (layer->GetExtent(&extent) != OGRERR_NONE) {
        return;
    }

    double dataWidth = extent.MaxX - extent.MinX;
    double dataHeight = extent.MaxY - extent.MinY;
    if (dataWidth <= 0 || dataHeight <= 0) {
        return;
    }

    // Render at high resolution for quality when zoomed
    int maxSize = std::max(width(), height()) * 2;
    maxSize = std::min(maxSize, 4096);

    int imgWidth, imgHeight;
    if (dataWidth > dataHeight) {
        imgWidth = maxSize;
        imgHeight = static_cast<int>(maxSize * dataHeight / dataWidth);
    } else {
        imgHeight = maxSize;
        imgWidth = static_cast<int>(maxSize * dataWidth / dataHeight);
    }
    imgWidth = std::max(imgWidth, 100);
    imgHeight = std::max(imgHeight, 100);

    QImage image(imgWidth, imgHeight, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    double scaleX = imgWidth / dataWidth;
    double scaleY = imgHeight / dataHeight;

    auto geoToPixel = [&](double geoX, double geoY) -> QPointF {
        double px = (geoX - extent.MinX) * scaleX;
        double py = imgHeight - (geoY - extent.MinY) * scaleY;
        return QPointF(px, py);
    };

    QPen outlinePen(QColor(180, 30, 30));
    outlinePen.setWidth(3);
    QBrush fillBrush(QColor(180, 30, 30, 100));
    QPen pointPen(QColor(30, 30, 180));
    pointPen.setWidth(8);
    pointPen.setCapStyle(Qt::RoundCap);

    painter.setPen(outlinePen);
    painter.setBrush(fillBrush);

    layer->ResetReading();
    OGRFeature *feature;

    while ((feature = layer->GetNextFeature()) != nullptr) {
        OGRGeometry *geometry = feature->GetGeometryRef();
        if (!geometry) {
            OGRFeature::DestroyFeature(feature);
            continue;
        }

        OGRwkbGeometryType geomType = wkbFlatten(geometry->getGeometryType());

        if (geomType == wkbPoint) {
            OGRPoint *point = geometry->toPoint();
            QPointF px = geoToPixel(point->getX(), point->getY());
            painter.setPen(pointPen);
            painter.drawPoint(px);
            painter.setPen(outlinePen);
        }
        else if (geomType == wkbLineString) {
            OGRLineString *line = geometry->toLineString();
            QPolygonF polyline;
            for (int i = 0; i < line->getNumPoints(); ++i) {
                polyline << geoToPixel(line->getX(i), line->getY(i));
            }
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(polyline);
            painter.setBrush(fillBrush);
        }
        else if (geomType == wkbPolygon) {
            OGRPolygon *polygon = geometry->toPolygon();
            OGRLinearRing *ring = polygon->getExteriorRing();
            if (ring) {
                QPolygonF poly;
                for (int i = 0; i < ring->getNumPoints(); ++i) {
                    poly << geoToPixel(ring->getX(i), ring->getY(i));
                }
                painter.drawPolygon(poly);
            }
        }
        else if (geomType == wkbMultiPoint) {
            OGRMultiPoint *multiPoint = geometry->toMultiPoint();
            painter.setPen(pointPen);
            for (int i = 0; i < multiPoint->getNumGeometries(); ++i) {
                OGRPoint *point = static_cast<OGRPoint*>(multiPoint->getGeometryRef(i));
                QPointF px = geoToPixel(point->getX(), point->getY());
                painter.drawPoint(px);
            }
            painter.setPen(outlinePen);
        }
        else if (geomType == wkbMultiLineString) {
            OGRMultiLineString *multiLine = geometry->toMultiLineString();
            painter.setBrush(Qt::NoBrush);
            for (int i = 0; i < multiLine->getNumGeometries(); ++i) {
                OGRLineString *line = static_cast<OGRLineString*>(multiLine->getGeometryRef(i));
                QPolygonF polyline;
                for (int j = 0; j < line->getNumPoints(); ++j) {
                    polyline << geoToPixel(line->getX(j), line->getY(j));
                }
                painter.drawPolyline(polyline);
            }
            painter.setBrush(fillBrush);
        }
        else if (geomType == wkbMultiPolygon) {
            OGRMultiPolygon *multiPoly = geometry->toMultiPolygon();
            for (int i = 0; i < multiPoly->getNumGeometries(); ++i) {
                OGRPolygon *polygon = static_cast<OGRPolygon*>(multiPoly->getGeometryRef(i));
                OGRLinearRing *ring = polygon->getExteriorRing();
                if (ring) {
                    QPolygonF poly;
                    for (int j = 0; j < ring->getNumPoints(); ++j) {
                        poly << geoToPixel(ring->getX(j), ring->getY(j));
                    }
                    painter.drawPolygon(poly);
                }
            }
        }

        OGRFeature::DestroyFeature(feature);
    }

    painter.end();
    m_vectorImage = image;
}

GeoTIFFInfo GISDisplayWidget::getGeoTIFFBounds()
{
    GeoTIFFInfo info;
    info.hasValidCRS = false;

    if (!m_rasterDataset) {
        return info;
    }

    info.pixelWidth = m_rasterDataset->GetRasterXSize();
    info.pixelHeight = m_rasterDataset->GetRasterYSize();

    if (m_rasterDataset->GetGeoTransform(info.geoTransform) != CE_None) {
        return info;
    }

    const char *projWkt = m_rasterDataset->GetProjectionRef();
    if (!projWkt || strlen(projWkt) == 0) {
        return info;
    }

    OGRSpatialReference srcSRS;
    if (srcSRS.importFromWkt(projWkt) != OGRERR_NONE) {
        return info;
    }

    OGRSpatialReference wgs84SRS;
    wgs84SRS.SetWellKnownGeogCS("WGS84");
    wgs84SRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srcSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRCoordinateTransformation *transform = OGRCreateCoordinateTransformation(&srcSRS, &wgs84SRS);
    if (!transform) {
        return info;
    }

    double corners[4][2] = {
        {info.geoTransform[0], info.geoTransform[3]},
        {info.geoTransform[0] + info.pixelWidth * info.geoTransform[1], info.geoTransform[3]},
        {info.geoTransform[0], info.geoTransform[3] + info.pixelHeight * info.geoTransform[5]},
        {info.geoTransform[0] + info.pixelWidth * info.geoTransform[1],
         info.geoTransform[3] + info.pixelHeight * info.geoTransform[5]}
    };

    info.minLon = 180.0;
    info.maxLon = -180.0;
    info.minLat = 90.0;
    info.maxLat = -90.0;

    for (int i = 0; i < 4; ++i) {
        double x = corners[i][0];
        double y = corners[i][1];
        if (transform->Transform(1, &x, &y)) {
            info.minLon = std::min(info.minLon, x);
            info.maxLon = std::max(info.maxLon, x);
            info.minLat = std::min(info.minLat, y);
            info.maxLat = std::max(info.maxLat, y);
        }
    }

    OGRCoordinateTransformation::DestroyCT(transform);
    info.hasValidCRS = (info.minLon < info.maxLon && info.minLat < info.maxLat);

    return info;
}

GeoJSONInfo GISDisplayWidget::getGeoJSONBounds(const QString &path)
{
    GeoJSONInfo info;
    info.hasValidBounds = false;

    GDALDataset *dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.toStdString().c_str(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr
    ));

    if (!dataset) {
        return info;
    }

    OGRLayer *layer = dataset->GetLayer(0);
    if (!layer) {
        GDALClose(dataset);
        return info;
    }

    OGREnvelope extent;
    if (layer->GetExtent(&extent) != OGRERR_NONE) {
        GDALClose(dataset);
        return info;
    }

    info.minLon = extent.MinX;
    info.maxLon = extent.MaxX;
    info.minLat = extent.MinY;
    info.maxLat = extent.MaxY;
    info.hasValidBounds = true;

    GDALClose(dataset);
    return info;
}

RGBBands GISDisplayWidget::selectRGBBands(GDALDataset *dataset)
{
    RGBBands bands;
    bands.fromWavelength = false;

    int bandCount = dataset->GetRasterCount();

    int ciRed = -1, ciGreen = -1, ciBlue = -1;
    for (int i = 1; i <= bandCount; ++i) {
        GDALRasterBand *band = dataset->GetRasterBand(i);
        GDALColorInterp ci = band->GetColorInterpretation();
        if (ci == GCI_RedBand) ciRed = i;
        else if (ci == GCI_GreenBand) ciGreen = i;
        else if (ci == GCI_BlueBand) ciBlue = i;
    }

    if (ciRed > 0 && ciGreen > 0 && ciBlue > 0) {
        bands.red = ciRed;
        bands.green = ciGreen;
        bands.blue = ciBlue;
        return bands;
    }

    std::vector<double> wavelengths = parseWavelengths(dataset);
    if (wavelengths.size() == static_cast<size_t>(bandCount) && bandCount >= 3) {
        int redBand = findClosestBand(wavelengths, TARGET_RED_NM);
        int greenBand = findClosestBand(wavelengths, TARGET_GREEN_NM);
        int blueBand = findClosestBand(wavelengths, TARGET_BLUE_NM);

        if (redBand > 0 && greenBand > 0 && blueBand > 0 &&
            redBand != greenBand && greenBand != blueBand && redBand != blueBand) {
            bands.red = redBand;
            bands.green = greenBand;
            bands.blue = blueBand;
            bands.fromWavelength = true;
            return bands;
        }
    }

    int margin = std::min(10, bandCount / 6);
    margin = std::max(margin, 1);
    int usableStart = margin + 1;
    int usableEnd = bandCount - margin;
    int usableRange = usableEnd - usableStart;

    if (usableRange < 3) {
        bands.red = std::min(3, bandCount);
        bands.green = std::min(2, bandCount);
        bands.blue = 1;
    } else {
        bands.red = usableStart + (usableRange * 2) / 3;
        bands.green = usableStart + usableRange / 2;
        bands.blue = usableStart + usableRange / 6;
    }

    return bands;
}

std::vector<double> GISDisplayWidget::parseWavelengths(GDALDataset *dataset)
{
    std::vector<double> wavelengths;

    const char* wavelengthKeys[] = {
        "wavelength", "Wavelength", "WAVELENGTH",
        "wavelengths", "Wavelengths", "WAVELENGTHS",
        "wl", "WL", "Wl", "lambda", "Lambda", "LAMBDA",
        nullptr
    };

    const char* metadataValue = nullptr;

    for (const char** key = wavelengthKeys; *key != nullptr; ++key) {
        metadataValue = dataset->GetMetadataItem(*key);
        if (metadataValue) break;
        metadataValue = dataset->GetMetadataItem(*key, "ENVI");
        if (metadataValue) break;
    }

    if (!metadataValue) {
        char **domains = dataset->GetMetadataDomainList();
        if (domains) {
            for (int i = 0; domains[i] != nullptr && !metadataValue; ++i) {
                for (const char** key = wavelengthKeys; *key != nullptr; ++key) {
                    metadataValue = dataset->GetMetadataItem(*key, domains[i]);
                    if (metadataValue) break;
                }
            }
            CSLDestroy(domains);
        }
    }

    if (!metadataValue) {
        return wavelengths;
    }

    QString wlStr = QString::fromUtf8(metadataValue);
    wlStr.remove('{').remove('}').remove('[').remove(']');
    QRegularExpression sep("[,;\\s]+");
    QStringList parts = wlStr.split(sep, Qt::SkipEmptyParts);

    for (const QString &part : parts) {
        bool ok;
        double val = part.toDouble(&ok);
        if (ok && val > 0) {
            wavelengths.push_back(val);
        }
    }

    if (!wavelengths.empty()) {
        double avgWl = 0;
        for (double w : wavelengths) avgWl += w;
        avgWl /= wavelengths.size();
        if (avgWl < 10.0) {
            for (double &w : wavelengths) w *= 1000.0;
        }
    }

    return wavelengths;
}

int GISDisplayWidget::findClosestBand(const std::vector<double> &wavelengths, double targetNm)
{
    int bestBand = -1;
    double bestDiff = std::numeric_limits<double>::max();

    for (size_t i = 0; i < wavelengths.size(); ++i) {
        double wl = wavelengths[i];
        if (wl >= VISIBLE_MIN_NM && wl <= VISIBLE_MAX_NM) {
            double diff = std::abs(wl - targetNm);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestBand = static_cast<int>(i) + 1;
            }
        }
    }

    return bestBand;
}

BandStatistics GISDisplayWidget::getCachedStatistics(GDALRasterBand *band)
{
    BandStatistics stats;
    stats.valid = false;

    CPLErr err = band->GetStatistics(FALSE, FALSE, &stats.min, &stats.max, &stats.mean, &stats.stddev);
    if (err == CE_None) {
        stats.valid = true;
    }

    return stats;
}

BandStatistics GISDisplayWidget::computeAndCacheStatistics(GDALRasterBand *band)
{
    BandStatistics stats;
    stats.valid = false;

    CPLErr err = band->ComputeStatistics(FALSE, &stats.min, &stats.max, &stats.mean, &stats.stddev, nullptr, nullptr);
    if (err == CE_None) {
        stats.valid = true;
    }

    return stats;
}

StretchParams GISDisplayWidget::stretchParamsFromStatistics(const BandStatistics &stats)
{
    StretchParams params;
    params.valid = false;

    if (!stats.valid) {
        params.warning = "No statistics available";
        return params;
    }

    const double zScore = 2.054;
    double approxMin = stats.mean - zScore * stats.stddev;
    double approxMax = stats.mean + zScore * stats.stddev;

    params.minP = std::max(approxMin, stats.min);
    params.maxP = std::min(approxMax, stats.max);

    double range = params.maxP - params.minP;
    if (range < 1e-10) {
        params.warning = "Statistics indicate constant data";
        return params;
    }

    double fullRange = stats.max - stats.min;
    if (fullRange > 1e-10 && range < fullRange * 0.1) {
        params.minP = stats.min;
        params.maxP = stats.max;
    }

    params.valid = true;
    return params;
}

StretchParams GISDisplayWidget::calculateStretchParams(std::vector<float> &data, double lowPct, double highPct)
{
    StretchParams params;
    params.valid = false;

    if (data.empty()) {
        params.warning = "Empty data";
        return params;
    }

    std::vector<float> sorted = data;
    std::sort(sorted.begin(), sorted.end());

    double dataMin = sorted.front();
    double dataMax = sorted.back();
    double dataRange = dataMax - dataMin;

    size_t lowIdx = static_cast<size_t>(lowPct * (sorted.size() - 1));
    size_t highIdx = static_cast<size_t>(highPct * (sorted.size() - 1));
    params.minP = sorted[lowIdx];
    params.maxP = sorted[highIdx];

    double stretchRange = params.maxP - params.minP;

    if (dataRange < 1e-10) {
        params.warning = "Constant data";
        return params;
    }

    if (stretchRange < dataRange * 0.1) {
        params.minP = dataMin;
        params.maxP = dataMax;
    }

    params.valid = true;
    return params;
}

uint8_t GISDisplayWidget::stretchValue(float val, double minP, double maxP)
{
    if (maxP <= minP) return 128;
    double normalized = (val - minP) / (maxP - minP);
    normalized = qBound(0.0, normalized, 1.0);
    return static_cast<uint8_t>(normalized * 255.0);
}

NoDataInfo GISDisplayWidget::getNoDataValue(GDALRasterBand *band)
{
    NoDataInfo info;
    int hasNoData = 0;
    info.value = band->GetNoDataValue(&hasNoData);
    info.hasNoData = (hasNoData != 0);
    return info;
}

bool GISDisplayWidget::isNoData(float pixelValue, const NoDataInfo &nodata)
{
    if (!nodata.hasNoData) return false;

    if (std::isnan(nodata.value)) {
        return std::isnan(pixelValue);
    }

    double tolerance = std::abs(nodata.value) * 1e-6;
    if (tolerance < 1e-10) tolerance = 1e-10;

    return std::abs(static_cast<double>(pixelValue) - nodata.value) < tolerance;
}
