#include "mainwindow.h"
#include <QVBoxLayout>
#include <QFileInfo>
#include <QPixmap>
#include <QProcess>
#include <QDebug>
#include <QApplication>
#include <QPainter>
#include <QPolygonF>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QStandardPaths>
#include <QDir>
#include <vector>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <QRegularExpression>
#include <ogrsf_frmts.h>
#include <ogr_spatialref.h>

// Calculate percentile value from sorted data (for histogram stretch)
static double percentile(std::vector<float> &data, double p) {
    if (data.empty()) return 0.0;
    std::sort(data.begin(), data.end());
    size_t idx = static_cast<size_t>(p * (data.size() - 1));
    return data[idx];
}

// Apply 2% histogram stretch: clip lowest/highest 2%, stretch rest to 0-255
static uint8_t stretchValue(float val, double minP, double maxP) {
    if (maxP <= minP) return 128;
    double normalized = (val - minP) / (maxP - minP);
    normalized = qBound(0.0, normalized, 1.0);
    return static_cast<uint8_t>(normalized * 255.0);
}

// Structure to hold stretch parameters and validation results
struct StretchParams {
    double minP;
    double maxP;
    bool valid;
    QString warning;
};

// Calculate stretch parameters with validation
// Returns valid=false if the range is too narrow for meaningful stretch
static StretchParams calculateStretchParams(std::vector<float> &data, double lowPct, double highPct) {
    StretchParams params;
    params.valid = false;
    params.minP = 0.0;
    params.maxP = 1.0;

    if (data.empty()) {
        params.warning = "Empty data";
        return params;
    }

    std::vector<float> sorted = data;
    std::sort(sorted.begin(), sorted.end());

    double dataMin = sorted.front();
    double dataMax = sorted.back();
    double dataRange = dataMax - dataMin;

    // Calculate percentiles
    size_t lowIdx = static_cast<size_t>(lowPct * (sorted.size() - 1));
    size_t highIdx = static_cast<size_t>(highPct * (sorted.size() - 1));
    params.minP = sorted[lowIdx];
    params.maxP = sorted[highIdx];

    double stretchRange = params.maxP - params.minP;

    // Validation: check if percentile range is meaningful
    if (dataRange < 1e-10) {
        // Constant data - no contrast possible
        params.warning = "Constant data (no variation)";
        return params;
    }

    // Check if stretch range covers at least 10% of data range
    // If not, fall back to min/max stretch
    if (stretchRange < dataRange * 0.1) {
        qDebug() << "Histogram stretch: percentile range too narrow ("
                 << stretchRange << " vs data range " << dataRange
                 << "), using full min/max range";
        params.minP = dataMin;
        params.maxP = dataMax;
        params.warning = "Used min/max fallback";
    }

    params.valid = true;
    return params;
}

// Post-stretch validation: check output histogram spread
// Returns true if contrast is adequate, false if image appears flat
static bool validateOutputContrast(const std::vector<uint8_t> &output) {
    if (output.empty()) return false;

    // Calculate histogram
    std::vector<int> histogram(256, 0);
    for (uint8_t val : output) {
        histogram[val]++;
    }

    // Find the range that contains 90% of pixels (5%-95%)
    size_t totalPixels = output.size();
    size_t cumulative = 0;
    int lowVal = 0, highVal = 255;

    for (int i = 0; i < 256; ++i) {
        cumulative += histogram[i];
        if (cumulative >= totalPixels * 0.05) {
            lowVal = i;
            break;
        }
    }

    cumulative = 0;
    for (int i = 255; i >= 0; --i) {
        cumulative += histogram[i];
        if (cumulative >= totalPixels * 0.05) {
            highVal = i;
            break;
        }
    }

    int outputRange = highVal - lowVal;

    // Contrast is adequate if 90% of pixels span at least 30% of 0-255 range
    bool adequate = outputRange >= 76;  // 76 = 30% of 255

    if (!adequate) {
        qDebug() << "Post-stretch validation: output contrast may be low"
                 << "(90% of pixels in range" << lowVal << "-" << highVal
                 << ", span=" << outputRange << ")";
    }

    return adequate;
}

// Structure to hold selected RGB band indices (1-based as per GDAL)
struct RGBBands {
    int red;
    int green;
    int blue;
    bool fromWavelength;  // true if selected based on wavelength metadata
};

// Target wavelengths for RGB (in nanometers)
static constexpr double TARGET_RED_NM = 665.0;    // Red ~650-700nm
static constexpr double TARGET_GREEN_NM = 560.0;  // Green ~520-560nm
static constexpr double TARGET_BLUE_NM = 470.0;   // Blue ~450-490nm
static constexpr double VISIBLE_MIN_NM = 380.0;
static constexpr double VISIBLE_MAX_NM = 780.0;

// Parse wavelength metadata from GDAL dataset
// Returns empty vector if no wavelength metadata found
static std::vector<double> parseWavelengths(GDALDataset *dataset) {
    std::vector<double> wavelengths;

    // List of possible metadata keys for wavelengths
    const char* wavelengthKeys[] = {
        "wavelength", "Wavelength", "WAVELENGTH",
        "wavelengths", "Wavelengths", "WAVELENGTHS",
        "wl", "WL", "Wl",
        "lambda", "Lambda", "LAMBDA",
        nullptr
    };

    const char* metadataValue = nullptr;

    // Check dataset metadata
    for (const char** key = wavelengthKeys; *key != nullptr; ++key) {
        metadataValue = dataset->GetMetadataItem(*key);
        if (metadataValue) break;

        // Also check in ENVI domain
        metadataValue = dataset->GetMetadataItem(*key, "ENVI");
        if (metadataValue) break;
    }

    if (!metadataValue) {
        // Check all metadata domains
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

    // Parse the wavelength string (could be comma, space, or brace separated)
    QString wlStr = QString::fromUtf8(metadataValue);
    // Remove braces and clean up
    wlStr.remove('{').remove('}').remove('[').remove(']');

    // Split by comma, space, or semicolon
    QRegularExpression sep("[,;\\s]+");
    QStringList parts = wlStr.split(sep, Qt::SkipEmptyParts);

    for (const QString &part : parts) {
        bool ok;
        double val = part.toDouble(&ok);
        if (ok && val > 0) {
            wavelengths.push_back(val);
        }
    }

    // Check if wavelengths might be in micrometers (typical range 0.4-2.5)
    // and convert to nanometers if needed
    if (!wavelengths.empty()) {
        double avgWl = 0;
        for (double w : wavelengths) avgWl += w;
        avgWl /= wavelengths.size();

        if (avgWl < 10.0) {
            // Likely in micrometers, convert to nm
            qDebug() << "Converting wavelengths from micrometers to nanometers";
            for (double &w : wavelengths) {
                w *= 1000.0;
            }
        }
    }

    qDebug() << "Found" << wavelengths.size() << "wavelengths in metadata";
    return wavelengths;
}

// Find the band index closest to the target wavelength
// Returns -1 if no suitable band found in visible range
static int findClosestBand(const std::vector<double> &wavelengths, double targetNm) {
    int bestBand = -1;
    double bestDiff = std::numeric_limits<double>::max();

    for (size_t i = 0; i < wavelengths.size(); ++i) {
        double wl = wavelengths[i];
        // Only consider visible range wavelengths
        if (wl >= VISIBLE_MIN_NM && wl <= VISIBLE_MAX_NM) {
            double diff = std::abs(wl - targetNm);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestBand = static_cast<int>(i) + 1;  // GDAL bands are 1-based
            }
        }
    }

    return bestBand;
}

// Select RGB bands for display from a multi-band GeoTIFF
static RGBBands selectRGBBands(GDALDataset *dataset) {
    RGBBands bands;
    bands.fromWavelength = false;

    int bandCount = dataset->GetRasterCount();

    // Priority 1: Check for GDAL color interpretation (GCI_RedBand, GCI_GreenBand, GCI_BlueBand)
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
        bands.fromWavelength = false;
        qDebug() << "Selected bands from color interpretation - R:" << bands.red
                 << "G:" << bands.green << "B:" << bands.blue;
        return bands;
    }

    // Priority 2: Try to find wavelength metadata
    std::vector<double> wavelengths = parseWavelengths(dataset);

    if (wavelengths.size() == static_cast<size_t>(bandCount) && bandCount >= 3) {
        // Try to find RGB bands based on wavelengths
        int redBand = findClosestBand(wavelengths, TARGET_RED_NM);
        int greenBand = findClosestBand(wavelengths, TARGET_GREEN_NM);
        int blueBand = findClosestBand(wavelengths, TARGET_BLUE_NM);

        // Check if we found valid and distinct bands
        if (redBand > 0 && greenBand > 0 && blueBand > 0 &&
            redBand != greenBand && greenBand != blueBand && redBand != blueBand) {
            bands.red = redBand;
            bands.green = greenBand;
            bands.blue = blueBand;
            bands.fromWavelength = true;

            qDebug() << "Selected bands from wavelengths - R:" << bands.red
                     << "(" << wavelengths[bands.red - 1] << "nm) G:" << bands.green
                     << "(" << wavelengths[bands.green - 1] << "nm) B:" << bands.blue
                     << "(" << wavelengths[bands.blue - 1] << "nm)";
            return bands;
        }
    }

    // Priority 3: Fallback - pick well-spread bands, avoiding first and last 5-10 bands
    // These edge bands are often noisy in hyperspectral data
    int margin = std::min(10, bandCount / 6);  // At least avoid some edge bands
    margin = std::max(margin, 1);

    int usableStart = margin + 1;              // 1-based
    int usableEnd = bandCount - margin;
    int usableRange = usableEnd - usableStart;

    if (usableRange < 3) {
        // Not enough bands to be picky, use first three
        bands.red = std::min(3, bandCount);
        bands.green = std::min(2, bandCount);
        bands.blue = 1;
    } else {
        // Pick well-spread bands within usable range
        // For natural color approximation: pick from upper, middle, lower third
        bands.red = usableStart + (usableRange * 2) / 3;   // Upper third (longer wavelength)
        bands.green = usableStart + usableRange / 2;        // Middle
        bands.blue = usableStart + usableRange / 6;         // Lower third (shorter wavelength)
    }

    qDebug() << "Selected fallback bands (no wavelength info) - R:" << bands.red
             << "G:" << bands.green << "B:" << bands.blue
             << "(total bands:" << bandCount << ", margin:" << margin << ")";

    return bands;
}

// Structure to hold full band statistics from .aux.xml
struct BandStatistics {
    double min;
    double max;
    double mean;
    double stddev;
    bool valid;
};

// Structure to hold NODATA value info
struct NoDataInfo {
    double value;
    bool hasNoData;
};

// Get NODATA value from a raster band
static NoDataInfo getNoDataValue(GDALRasterBand *band) {
    NoDataInfo info;
    int hasNoData = 0;
    info.value = band->GetNoDataValue(&hasNoData);
    info.hasNoData = (hasNoData != 0);

    if (info.hasNoData) {
        qDebug() << "Band has NODATA value:" << info.value;
    }

    return info;
}

// Check if a pixel value matches the NODATA value (with tolerance for float comparison)
static bool isNoData(float pixelValue, const NoDataInfo &nodata) {
    if (!nodata.hasNoData) return false;

    // Use tolerance for floating point comparison
    // Handle NaN NODATA values
    if (std::isnan(nodata.value)) {
        return std::isnan(pixelValue);
    }

    // For regular values, use relative tolerance
    double tolerance = std::abs(nodata.value) * 1e-6;
    if (tolerance < 1e-10) tolerance = 1e-10;

    return std::abs(static_cast<double>(pixelValue) - nodata.value) < tolerance;
}

// Get cached statistics from .aux.xml sidecar file
// Returns valid=false if statistics are not cached
static BandStatistics getCachedStatistics(GDALRasterBand *band) {
    BandStatistics stats;
    stats.valid = false;

    // Try to get existing statistics (bApproxOK=FALSE, bForce=FALSE)
    // This returns CE_None only if statistics are already cached in .aux.xml
    CPLErr err = band->GetStatistics(FALSE, FALSE, &stats.min, &stats.max, &stats.mean, &stats.stddev);

    if (err == CE_None) {
        stats.valid = true;
        qDebug() << "Using cached statistics: min=" << stats.min << "max=" << stats.max
                 << "mean=" << stats.mean << "stddev=" << stats.stddev;
    }

    return stats;
}

// Compute statistics and cache them in .aux.xml sidecar file
static BandStatistics computeAndCacheStatistics(GDALRasterBand *band) {
    BandStatistics stats;
    stats.valid = false;

    qDebug() << "Computing statistics (will be cached in .aux.xml)...";
    CPLErr err = band->ComputeStatistics(FALSE, &stats.min, &stats.max, &stats.mean, &stats.stddev, nullptr, nullptr);

    if (err == CE_None) {
        stats.valid = true;
        qDebug() << "Computed and cached statistics: min=" << stats.min << "max=" << stats.max
                 << "mean=" << stats.mean << "stddev=" << stats.stddev;
    } else {
        qDebug() << "Failed to compute statistics";
    }

    return stats;
}

// Calculate 2%/98% stretch parameters from cached statistics
// Uses normal distribution approximation: percentile ≈ mean ± k*stddev
// For 2%/98%, k ≈ 2.05 (from inverse normal CDF)
static StretchParams stretchParamsFromStatistics(const BandStatistics &stats) {
    StretchParams params;
    params.valid = false;

    if (!stats.valid) {
        params.warning = "No statistics available";
        return params;
    }

    // Approximate 2nd and 98th percentiles using normal distribution
    // z-score for 2% = -2.054, for 98% = +2.054
    const double zScore = 2.054;

    double approxMin = stats.mean - zScore * stats.stddev;
    double approxMax = stats.mean + zScore * stats.stddev;

    // Clamp to actual data range
    params.minP = std::max(approxMin, stats.min);
    params.maxP = std::min(approxMax, stats.max);

    // Validate the range
    double range = params.maxP - params.minP;
    if (range < 1e-10) {
        params.warning = "Statistics indicate constant data";
        return params;
    }

    // Check if the approximated range is reasonable (at least 10% of full range)
    double fullRange = stats.max - stats.min;
    if (fullRange > 1e-10 && range < fullRange * 0.1) {
        // Range too narrow, use full min/max
        params.minP = stats.min;
        params.maxP = stats.max;
        qDebug() << "Statistics-based stretch range too narrow, using full range";
    }

    params.valid = true;
    qDebug() << "Stretch params from statistics: minP=" << params.minP << "maxP=" << params.maxP;
    return params;
}

// Maximum size for GeoJSON rendering
static constexpr int GEOJSON_MAX_RENDER_SIZE = 5000;

// Get sidecar preview path for a GeoJSON file
// Standard convention: filename.geojson.png
static QString getGeoJSONPreviewPath(const QString &geojsonPath) {
    return geojsonPath + ".png";
}

// Render a GeoJSON file to a QImage
// Returns null QImage on failure
static QImage renderGeoJSON(const QString &path, int maxSize = GEOJSON_MAX_RENDER_SIZE) {
    GDALDataset *dataset = (GDALDataset*)GDALOpenEx(
        path.toStdString().c_str(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr
    );

    if (!dataset) {
        qDebug() << "Failed to open GeoJSON:" << path;
        return QImage();
    }

    // Get the first layer
    OGRLayer *layer = dataset->GetLayer(0);
    if (!layer) {
        qDebug() << "No layers in GeoJSON";
        GDALClose(dataset);
        return QImage();
    }

    // Get extent
    OGREnvelope extent;
    if (layer->GetExtent(&extent) != OGRERR_NONE) {
        qDebug() << "Failed to get GeoJSON extent";
        GDALClose(dataset);
        return QImage();
    }

    double dataWidth = extent.MaxX - extent.MinX;
    double dataHeight = extent.MaxY - extent.MinY;

    if (dataWidth <= 0 || dataHeight <= 0) {
        qDebug() << "Invalid GeoJSON extent";
        GDALClose(dataset);
        return QImage();
    }

    // Calculate image size (maintain aspect ratio, max dimension = maxSize)
    int imgWidth, imgHeight;
    if (dataWidth > dataHeight) {
        imgWidth = maxSize;
        imgHeight = static_cast<int>(maxSize * dataHeight / dataWidth);
    } else {
        imgHeight = maxSize;
        imgWidth = static_cast<int>(maxSize * dataWidth / dataHeight);
    }

    // Ensure minimum size
    imgWidth = std::max(imgWidth, 100);
    imgHeight = std::max(imgHeight, 100);

    qDebug() << "Rendering GeoJSON to" << imgWidth << "x" << imgHeight;

    // Create image with white background
    QImage image(imgWidth, imgHeight, QImage::Format_ARGB32);
    image.fill(QColor(30, 30, 30));  // Dark background matching app theme

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Calculate transform from geo coordinates to pixel coordinates
    double scaleX = imgWidth / dataWidth;
    double scaleY = imgHeight / dataHeight;

    // Lambda to transform coordinates
    auto geoToPixel = [&](double geoX, double geoY) -> QPointF {
        double px = (geoX - extent.MinX) * scaleX;
        double py = imgHeight - (geoY - extent.MinY) * scaleY;  // Y is flipped
        return QPointF(px, py);
    };

    // Style settings
    QPen outlinePen(QColor(65, 165, 238));  // Blue outline
    outlinePen.setWidth(2);
    QBrush fillBrush(QColor(65, 165, 238, 80));  // Semi-transparent blue fill
    QPen pointPen(QColor(238, 130, 65));  // Orange for points
    pointPen.setWidth(6);
    pointPen.setCapStyle(Qt::RoundCap);

    painter.setPen(outlinePen);
    painter.setBrush(fillBrush);

    // Iterate through features
    layer->ResetReading();
    OGRFeature *feature;
    int featureCount = 0;

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
        featureCount++;
    }

    painter.end();
    GDALClose(dataset);

    qDebug() << "Rendered" << featureCount << "features from GeoJSON";
    return image;
}

// Load or render GeoJSON preview, using cached sidecar if available
static QImage loadGeoJSONPreview(const QString &path) {
    QString previewPath = getGeoJSONPreviewPath(path);
    QFileInfo previewInfo(previewPath);
    QFileInfo sourceInfo(path);

    // Check if cached preview exists and is newer than source
    if (previewInfo.exists() && previewInfo.lastModified() >= sourceInfo.lastModified()) {
        qDebug() << "Loading cached GeoJSON preview:" << previewPath;
        QImage cached(previewPath);
        if (!cached.isNull()) {
            return cached;
        }
        qDebug() << "Cached preview invalid, regenerating";
    }

    // Render and cache
    qDebug() << "Rendering GeoJSON preview...";
    QImage rendered = renderGeoJSON(path);

    if (!rendered.isNull()) {
        // Save as PNG sidecar file
        if (rendered.save(previewPath, "PNG")) {
            qDebug() << "Cached GeoJSON preview to:" << previewPath;
        } else {
            qDebug() << "Failed to cache GeoJSON preview";
        }
    }

    return rendered;
}

// ============== OSM Background Tile Support ==============

// OSM tile size in pixels
static constexpr int OSM_TILE_SIZE = 256;

// Get tile cache directory
static QString getOSMTileCacheDir() {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/osm_tiles";
    QDir().mkpath(cacheDir);
    return cacheDir;
}

// Convert longitude to tile X coordinate
static int lonToTileX(double lon, int zoom) {
    return static_cast<int>(std::floor((lon + 180.0) / 360.0 * (1 << zoom)));
}

// Convert latitude to tile Y coordinate
static int latToTileY(double lat, int zoom) {
    double latRad = lat * M_PI / 180.0;
    return static_cast<int>(std::floor((1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * (1 << zoom)));
}

// Convert tile X to longitude (west edge)
static double tileXToLon(int x, int zoom) {
    return x / static_cast<double>(1 << zoom) * 360.0 - 180.0;
}

// Convert tile Y to latitude (north edge)
static double tileYToLat(int y, int zoom) {
    double n = M_PI - 2.0 * M_PI * y / static_cast<double>(1 << zoom);
    return 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

// Fetch a single OSM tile (with caching)
static QImage fetchOSMTile(int x, int y, int zoom) {
    QString cacheDir = getOSMTileCacheDir();
    QString cachePath = QString("%1/%2_%3_%4.png").arg(cacheDir).arg(zoom).arg(x).arg(y);

    // Check cache first
    if (QFileInfo::exists(cachePath)) {
        QImage cached(cachePath);
        if (!cached.isNull()) {
            return cached;
        }
    }

    // Fetch from OSM server
    QString url = QString("https://tile.openstreetmap.org/%1/%2/%3.png").arg(zoom).arg(x).arg(y);

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "WorkspaceApp/1.0");

    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QImage tile;
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        tile.loadFromData(data);

        // Cache the tile
        if (!tile.isNull()) {
            tile.save(cachePath, "PNG");
        }
    } else {
        qDebug() << "Failed to fetch OSM tile:" << reply->errorString();
    }

    reply->deleteLater();
    return tile;
}

// Structure to hold GeoTIFF geographic info
struct GeoTIFFInfo {
    double minLon, maxLon, minLat, maxLat;  // WGS84 bounds
    bool hasValidCRS;
    int pixelWidth, pixelHeight;
    double geoTransform[6];
};

// Get geographic bounds of a GeoTIFF in WGS84
static GeoTIFFInfo getGeoTIFFBounds(GDALDataset *dataset) {
    GeoTIFFInfo info;
    info.hasValidCRS = false;
    info.pixelWidth = dataset->GetRasterXSize();
    info.pixelHeight = dataset->GetRasterYSize();

    // Get geotransform
    if (dataset->GetGeoTransform(info.geoTransform) != CE_None) {
        qDebug() << "No geotransform in GeoTIFF";
        return info;
    }

    // Get projection
    const char *projWkt = dataset->GetProjectionRef();
    if (!projWkt || strlen(projWkt) == 0) {
        qDebug() << "No projection in GeoTIFF";
        return info;
    }

    // Create source spatial reference
    OGRSpatialReference srcSRS;
    if (srcSRS.importFromWkt(projWkt) != OGRERR_NONE) {
        qDebug() << "Failed to parse GeoTIFF projection";
        return info;
    }

    // Create WGS84 target
    OGRSpatialReference wgs84SRS;
    wgs84SRS.SetWellKnownGeogCS("WGS84");
    wgs84SRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srcSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    // Create coordinate transformation
    OGRCoordinateTransformation *transform = OGRCreateCoordinateTransformation(&srcSRS, &wgs84SRS);
    if (!transform) {
        qDebug() << "Failed to create coordinate transformation";
        return info;
    }

    // Calculate corner coordinates in source CRS
    double corners[4][2] = {
        {info.geoTransform[0], info.geoTransform[3]},  // Upper-left
        {info.geoTransform[0] + info.pixelWidth * info.geoTransform[1], info.geoTransform[3]},  // Upper-right
        {info.geoTransform[0], info.geoTransform[3] + info.pixelHeight * info.geoTransform[5]},  // Lower-left
        {info.geoTransform[0] + info.pixelWidth * info.geoTransform[1],
         info.geoTransform[3] + info.pixelHeight * info.geoTransform[5]}  // Lower-right
    };

    // Transform to WGS84 and find bounds
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
    if (info.hasValidCRS) {
        qDebug() << "GeoTIFF bounds (WGS84): lon" << info.minLon << "-" << info.maxLon
                 << "lat" << info.minLat << "-" << info.maxLat;
    }

    return info;
}

// Calculate appropriate zoom level for given extent and display size
static int calculateOSMZoom(double minLon, double maxLon, double minLat, double maxLat, int displayWidth, int displayHeight) {
    for (int zoom = 18; zoom >= 0; --zoom) {
        int minTileX = lonToTileX(minLon, zoom);
        int maxTileX = lonToTileX(maxLon, zoom);
        int minTileY = latToTileY(maxLat, zoom);  // Note: Y is inverted
        int maxTileY = latToTileY(minLat, zoom);

        int tilesX = maxTileX - minTileX + 1;
        int tilesY = maxTileY - minTileY + 1;

        // Check if this zoom level gives reasonable coverage
        if (tilesX * OSM_TILE_SIZE <= displayWidth * 2 && tilesY * OSM_TILE_SIZE <= displayHeight * 2) {
            return zoom;
        }
    }
    return 4;  // Default to low zoom
}

// Create OSM background for a given geographic extent
static QImage createOSMBackground(double minLon, double maxLon, double minLat, double maxLat, int displayWidth, int displayHeight) {
    int zoom = calculateOSMZoom(minLon, maxLon, minLat, maxLat, displayWidth, displayHeight);

    // Get tile range
    int minTileX = lonToTileX(minLon, zoom);
    int maxTileX = lonToTileX(maxLon, zoom);
    int minTileY = latToTileY(maxLat, zoom);
    int maxTileY = latToTileY(minLat, zoom);

    // Add buffer tiles for background
    minTileX = std::max(0, minTileX - 1);
    maxTileX = std::min((1 << zoom) - 1, maxTileX + 1);
    minTileY = std::max(0, minTileY - 1);
    maxTileY = std::min((1 << zoom) - 1, maxTileY + 1);

    int tilesX = maxTileX - minTileX + 1;
    int tilesY = maxTileY - minTileY + 1;

    qDebug() << "Fetching OSM tiles: zoom" << zoom << "tiles" << tilesX << "x" << tilesY;

    // Create composite image from tiles
    QImage composite(tilesX * OSM_TILE_SIZE, tilesY * OSM_TILE_SIZE, QImage::Format_ARGB32);
    composite.fill(QColor(30, 30, 30));

    QPainter painter(&composite);
    for (int ty = minTileY; ty <= maxTileY; ++ty) {
        for (int tx = minTileX; tx <= maxTileX; ++tx) {
            QImage tile = fetchOSMTile(tx, ty, zoom);
            if (!tile.isNull()) {
                int px = (tx - minTileX) * OSM_TILE_SIZE;
                int py = (ty - minTileY) * OSM_TILE_SIZE;
                painter.drawImage(px, py, tile);
            }
        }
    }
    painter.end();

    // Calculate the pixel bounds of the data extent within the tile mosaic
    double tileMinLon = tileXToLon(minTileX, zoom);
    double tileMaxLon = tileXToLon(maxTileX + 1, zoom);
    double tileMaxLat = tileYToLat(minTileY, zoom);
    double tileMinLat = tileYToLat(maxTileY + 1, zoom);

    // Store extent info for later use (returned as metadata in the image)
    // We'll encode the geo extent in the image's text metadata
    composite.setText("minLon", QString::number(tileMinLon, 'f', 10));
    composite.setText("maxLon", QString::number(tileMaxLon, 'f', 10));
    composite.setText("minLat", QString::number(tileMinLat, 'f', 10));
    composite.setText("maxLat", QString::number(tileMaxLat, 'f', 10));

    return composite;
}

// Composite GeoTIFF over OSM background, filling the display panel
static QImage compositeWithOSMBackground(const QImage &geotiff, const GeoTIFFInfo &info, int displayWidth, int displayHeight) {
    if (!info.hasValidCRS) {
        return geotiff;  // No CRS info, can't add OSM background
    }

    if (displayWidth <= 0 || displayHeight <= 0) {
        return geotiff;
    }

    // Calculate the geographic extent needed to fill the display panel
    // while keeping the GeoTIFF centered
    double geoLonRange = info.maxLon - info.minLon;
    double geoLatRange = info.maxLat - info.minLat;
    double geoCenterLon = (info.minLon + info.maxLon) / 2.0;
    double geoCenterLat = (info.minLat + info.maxLat) / 2.0;

    // Calculate aspect ratios
    double displayAspect = static_cast<double>(displayWidth) / displayHeight;
    double geoAspect = geoLonRange / geoLatRange;

    // Expand the geographic extent to match display aspect ratio
    // and add buffer so GeoTIFF doesn't touch edges
    double bgLonRange, bgLatRange;
    double bufferFactor = 1.5;  // GeoTIFF will occupy ~67% of the view

    if (geoAspect > displayAspect) {
        // GeoTIFF is wider than display - expand latitude
        bgLonRange = geoLonRange * bufferFactor;
        bgLatRange = bgLonRange / displayAspect;
    } else {
        // GeoTIFF is taller than display - expand longitude
        bgLatRange = geoLatRange * bufferFactor;
        bgLonRange = bgLatRange * displayAspect;
    }

    double bgMinLon = std::max(-180.0, geoCenterLon - bgLonRange / 2.0);
    double bgMaxLon = std::min(180.0, geoCenterLon + bgLonRange / 2.0);
    double bgMinLat = std::max(-85.0, geoCenterLat - bgLatRange / 2.0);
    double bgMaxLat = std::min(85.0, geoCenterLat + bgLatRange / 2.0);

    // Recalculate actual ranges after clamping
    bgLonRange = bgMaxLon - bgMinLon;
    bgLatRange = bgMaxLat - bgMinLat;

    // Get OSM background tiles
    QImage tileBackground = createOSMBackground(bgMinLon, bgMaxLon, bgMinLat, bgMaxLat, displayWidth, displayHeight);
    if (tileBackground.isNull()) {
        return geotiff;
    }

    // Create final image at exact display size
    QImage finalImage(displayWidth, displayHeight, QImage::Format_ARGB32);
    finalImage.fill(QColor(30, 30, 30));

    // Scale the tile background to fill the display
    QImage scaledBackground = tileBackground.scaled(displayWidth, displayHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QPainter painter(&finalImage);
    painter.drawImage(0, 0, scaledBackground);

    // Calculate where the GeoTIFF should be placed on the final image
    double leftPct = (info.minLon - bgMinLon) / bgLonRange;
    double rightPct = (info.maxLon - bgMinLon) / bgLonRange;
    double topPct = (bgMaxLat - info.maxLat) / bgLatRange;
    double bottomPct = (bgMaxLat - info.minLat) / bgLatRange;

    int destX = static_cast<int>(leftPct * displayWidth);
    int destY = static_cast<int>(topPct * displayHeight);
    int destWidth = static_cast<int>((rightPct - leftPct) * displayWidth);
    int destHeight = static_cast<int>((bottomPct - topPct) * displayHeight);

    // Ensure minimum size
    destWidth = std::max(destWidth, 10);
    destHeight = std::max(destHeight, 10);

    // Scale and draw the GeoTIFF
    QImage scaledGeotiff = geotiff.scaled(destWidth, destHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    painter.drawImage(destX, destY, scaledGeotiff);
    painter.end();

    qDebug() << "Composited GeoTIFF over OSM background at" << displayWidth << "x" << displayHeight;
    return finalImage;
}

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
    
    // Image preview with scroll area for zoom/pan support
    imagePreview = new QLabel("Select a file...");
    imagePreview->setAlignment(Qt::AlignCenter);
    imagePreview->setStyleSheet("background: #1e1e1e;");
    imagePreview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    scrollArea = new QScrollArea(this);
    scrollArea->setWidget(imagePreview);
    scrollArea->setWidgetResizable(false);  // We control sizing for zoom
    scrollArea->setAlignment(Qt::AlignCenter);
    scrollArea->setMinimumSize(400, 300);
    scrollArea->setStyleSheet("border: 1px solid #555; background: #1e1e1e;");
    scrollArea->installEventFilter(this);  // Monitor resize events for image scaling

    zoomFactor = 0.0;  // 0.0 = fit to window mode
    
    // Terminal (25%) - minimum height ensures more than one line visible
    terminal = new QTermWidget(this);
    terminal->setShellProgram("/bin/bash");
    terminal->setWorkingDirectory(QDir::homePath());
    terminal->setColorScheme("Linux");  // Black background color scheme
    terminal->setMinimumHeight(100);    // Ensure terminal has reasonable minimum height
    terminal->startShellProgram();

    // Install application-level event filter to intercept Ctrl+Shift+C/V
    // QTermWidget's internal view consumes key events, so we catch them at app level
    qApp->installEventFilter(this);

    // Splitter layout - terminal at bottom
    topSplitter = new QSplitter(Qt::Horizontal, this);
    topSplitter->addWidget(fileTree);
    topSplitter->addWidget(scrollArea);
    topSplitter->setStretchFactor(0, 1);   // File tree: 33%
    topSplitter->setStretchFactor(1, 2);   // Preview: 67%

    splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(topSplitter);
    splitter->addWidget(terminal);
    splitter->setStretchFactor(0, 11);  // Top area: ~69%
    splitter->setStretchFactor(1, 5);   // Terminal: ~31% (25% + 25% increase)
    
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
        imagePreview->setText("Directory: " + info.fileName());
    } else if (info.isFile()) {
        changeTerminalDir(info.absolutePath());
        loadImage(path);
    }
}

void MainWindow::loadImage(const QString &path) {
    QFileInfo info(path);
    QString ext = info.suffix().toLower();

    if (ext == "tif" || ext == "tiff") {
        // GDAL GeoTIFF handling
        GDALDataset *dataset = (GDALDataset*)GDALOpen(path.toStdString().c_str(), GA_ReadOnly);
        if (dataset) {
            int width = dataset->GetRasterXSize();
            int height = dataset->GetRasterYSize();
            int bandCount = dataset->GetRasterCount();

            // Use ARGB32 for transparency support (NODATA pixels)
            QImage image(width, height, QImage::Format_ARGB32);
            image.fill(Qt::transparent);

            bool readSuccess = false;

            if (bandCount >= 3) {
                // Multi-band image: select appropriate RGB bands
                RGBBands rgbBands = selectRGBBands(dataset);

                size_t pixelCount = static_cast<size_t>(width) * height;
                std::vector<float> r(pixelCount), g(pixelCount), b(pixelCount);
                GDALRasterBand *bandR = dataset->GetRasterBand(rgbBands.red);
                GDALRasterBand *bandG = dataset->GetRasterBand(rgbBands.green);
                GDALRasterBand *bandB = dataset->GetRasterBand(rgbBands.blue);

                // Get NODATA values for each band
                NoDataInfo nodataR = getNoDataValue(bandR);
                NoDataInfo nodataG = getNoDataValue(bandG);
                NoDataInfo nodataB = getNoDataValue(bandB);

                // Try to get stretch parameters from cached statistics (.aux.xml)
                BandStatistics statsR = getCachedStatistics(bandR);
                BandStatistics statsG = getCachedStatistics(bandG);
                BandStatistics statsB = getCachedStatistics(bandB);

                // If not cached, compute and cache statistics
                if (!statsR.valid) statsR = computeAndCacheStatistics(bandR);
                if (!statsG.valid) statsG = computeAndCacheStatistics(bandG);
                if (!statsB.valid) statsB = computeAndCacheStatistics(bandB);

                // Calculate stretch parameters from statistics
                StretchParams paramsR = stretchParamsFromStatistics(statsR);
                StretchParams paramsG = stretchParamsFromStatistics(statsG);
                StretchParams paramsB = stretchParamsFromStatistics(statsB);

                // Read pixel data for display
                CPLErr errR = bandR->RasterIO(GF_Read, 0, 0, width, height, r.data(), width, height, GDT_Float32, 0, 0);
                CPLErr errG = bandG->RasterIO(GF_Read, 0, 0, width, height, g.data(), width, height, GDT_Float32, 0, 0);
                CPLErr errB = bandB->RasterIO(GF_Read, 0, 0, width, height, b.data(), width, height, GDT_Float32, 0, 0);

                if (errR == CE_None && errG == CE_None && errB == CE_None) {
                    // If statistics-based params failed, fall back to pixel-based percentiles
                    if (!paramsR.valid) {
                        qDebug() << "Falling back to pixel-based percentiles for R band";
                        paramsR = calculateStretchParams(r, 0.02, 0.98);
                    }
                    if (!paramsG.valid) {
                        qDebug() << "Falling back to pixel-based percentiles for G band";
                        paramsG = calculateStretchParams(g, 0.02, 0.98);
                    }
                    if (!paramsB.valid) {
                        qDebug() << "Falling back to pixel-based percentiles for B band";
                        paramsB = calculateStretchParams(b, 0.02, 0.98);
                    }

                    if (paramsR.valid && paramsG.valid && paramsB.valid) {
                        // Apply stretch and collect output for validation
                        std::vector<uint8_t> convergenceCheck;
                        convergenceCheck.reserve(pixelCount);

                        for (int y = 0; y < height; ++y) {
                            QRgb *scanline = reinterpret_cast<QRgb*>(image.scanLine(y));
                            for (int x = 0; x < width; ++x) {
                                size_t idx = static_cast<size_t>(y) * width + x;

                                // Check for NODATA in any band - make pixel transparent
                                if (isNoData(r[idx], nodataR) || isNoData(g[idx], nodataG) || isNoData(b[idx], nodataB)) {
                                    scanline[x] = qRgba(0, 0, 0, 0);  // Fully transparent
                                    continue;
                                }

                                uint8_t rv = stretchValue(r[idx], paramsR.minP, paramsR.maxP);
                                uint8_t gv = stretchValue(g[idx], paramsG.minP, paramsG.maxP);
                                uint8_t bv = stretchValue(b[idx], paramsB.minP, paramsB.maxP);
                                scanline[x] = qRgba(rv, gv, bv, 255);  // Fully opaque
                                // Use luminance for contrast check
                                convergenceCheck.push_back(static_cast<uint8_t>(0.299 * rv + 0.587 * gv + 0.114 * bv));
                            }
                        }

                        // Post-stretch validation
                        if (!validateOutputContrast(convergenceCheck)) {
                            qDebug() << "RGB stretch: contrast validation warning for" << path;
                        }
                        readSuccess = true;
                    } else {
                        qDebug() << "RGB stretch failed:" << paramsR.warning << paramsG.warning << paramsB.warning;
                    }
                }
            } else if (bandCount == 1) {
                // Grayscale with 2% histogram stretch
                GDALRasterBand *band = dataset->GetRasterBand(1);
                size_t pixelCount = static_cast<size_t>(width) * height;

                // Get NODATA value
                NoDataInfo nodata = getNoDataValue(band);

                // Try to get stretch parameters from cached statistics (.aux.xml)
                BandStatistics stats = getCachedStatistics(band);
                if (!stats.valid) {
                    stats = computeAndCacheStatistics(band);
                }

                // Calculate stretch parameters from statistics
                StretchParams params = stretchParamsFromStatistics(stats);

                std::vector<float> data(pixelCount);
                CPLErr errRead = band->RasterIO(GF_Read, 0, 0, width, height, data.data(), width, height, GDT_Float32, 0, 0);

                if (errRead == CE_None) {
                    // If statistics-based params failed, fall back to pixel-based percentiles
                    if (!params.valid) {
                        qDebug() << "Falling back to pixel-based percentiles for grayscale";
                        params = calculateStretchParams(data, 0.02, 0.98);
                    }

                    if (params.valid) {
                        std::vector<uint8_t> output;
                        output.reserve(pixelCount);

                        for (int y = 0; y < height; ++y) {
                            QRgb *scanline = reinterpret_cast<QRgb*>(image.scanLine(y));
                            for (int x = 0; x < width; ++x) {
                                size_t idx = static_cast<size_t>(y) * width + x;

                                // Check for NODATA - make pixel transparent
                                if (isNoData(data[idx], nodata)) {
                                    scanline[x] = qRgba(0, 0, 0, 0);  // Fully transparent
                                    continue;
                                }

                                uint8_t gray = stretchValue(data[idx], params.minP, params.maxP);
                                scanline[x] = qRgba(gray, gray, gray, 255);  // Fully opaque
                                output.push_back(gray);
                            }
                        }

                        // Post-stretch validation
                        if (!validateOutputContrast(output)) {
                            qDebug() << "Grayscale stretch: contrast validation warning for" << path;
                        }
                        readSuccess = true;
                    } else {
                        qDebug() << "Grayscale stretch failed:" << params.warning;
                    }
                }
            }

            if (readSuccess) {
                // Get geographic bounds for OSM background
                GeoTIFFInfo geoInfo = getGeoTIFFBounds(dataset);

                // Get display size for OSM background calculation
                QSize displaySize = scrollArea->viewport()->size();

                // Composite with OSM background if we have valid CRS
                QImage finalImage = compositeWithOSMBackground(image, geoInfo, displaySize.width(), displaySize.height());

                currentPixmap = QPixmap::fromImage(finalImage);
                updateImageDisplay();
                lastImagePath = path;
            } else {
                imagePreview->setText("Failed to read GeoTIFF raster data");
            }
            GDALClose(dataset);
        } else {
            imagePreview->setText("Failed to open GeoTIFF");
        }
    } else if (ext == "geojson" || ext == "json") {
        // GeoJSON vector data - render to image
        QImage rendered = loadGeoJSONPreview(path);
        if (!rendered.isNull()) {
            currentPixmap = QPixmap::fromImage(rendered);
            updateImageDisplay();
            lastImagePath = path;
        } else {
            currentPixmap = QPixmap();
            imagePreview->setText("Failed to render GeoJSON: " + info.fileName());
        }
    } else {
        // Regular images
        QPixmap pixmap(path);
        if (!pixmap.isNull()) {
            currentPixmap = pixmap;  // Store original for proper resizing
            updateImageDisplay();
            lastImagePath = path;
        } else {
            currentPixmap = QPixmap();  // Clear stored pixmap
            imagePreview->setText("No preview: " + info.fileName());
        }
    }
}

void MainWindow::updateImageDisplay() {
    if (currentPixmap.isNull()) {
        return;
    }

    QSize scaledSize;
    if (zoomFactor <= 0.0) {
        // Fit to window mode
        QSize availableSize = scrollArea->viewport()->size();
        scaledSize = currentPixmap.size().scaled(availableSize, Qt::KeepAspectRatio);
    } else {
        // Fixed zoom level
        scaledSize = currentPixmap.size() * zoomFactor;
    }

    QPixmap scaled = currentPixmap.scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    imagePreview->setPixmap(scaled);
    imagePreview->resize(scaled.size());
}

void MainWindow::refreshDisplay() {
    if (!lastImagePath.isEmpty() && QFileInfo::exists(lastImagePath)) {
        loadImage(lastImagePath);
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    updateImageDisplay();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == scrollArea && event->type() == QEvent::Resize) {
        if (zoomFactor <= 0.0) {  // Only auto-resize in fit-to-window mode
            updateImageDisplay();
        }
    }

    // Intercept Ctrl+Shift+C/V for terminal clipboard operations
    // QTermWidget's internal view consumes key events, so we catch them at app level
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            // Check if focus is on terminal or one of its children
            QWidget *focusWidget = QApplication::focusWidget();
            bool terminalHasFocus = focusWidget && (focusWidget == terminal || terminal->isAncestorOf(focusWidget));

            if (terminalHasFocus) {
                if (keyEvent->key() == Qt::Key_C) {
                    terminal->copyClipboard();
                    return true;  // Event handled
                } else if (keyEvent->key() == Qt::Key_V) {
                    terminal->pasteClipboard();
                    return true;  // Event handled
                }
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::zoomIn() {
    if (currentPixmap.isNull()) return;

    if (zoomFactor <= 0.0) {
        // Switching from fit-to-window to fixed zoom
        QSize viewportSize = scrollArea->viewport()->size();
        QSize scaledSize = currentPixmap.size().scaled(viewportSize, Qt::KeepAspectRatio);
        zoomFactor = static_cast<double>(scaledSize.width()) / currentPixmap.width();
    }
    zoomFactor *= 1.25;
    if (zoomFactor > 10.0) zoomFactor = 10.0;  // Max 1000% zoom
    updateImageDisplay();
}

void MainWindow::zoomOut() {
    if (currentPixmap.isNull()) return;

    if (zoomFactor <= 0.0) {
        // Switching from fit-to-window to fixed zoom
        QSize viewportSize = scrollArea->viewport()->size();
        QSize scaledSize = currentPixmap.size().scaled(viewportSize, Qt::KeepAspectRatio);
        zoomFactor = static_cast<double>(scaledSize.width()) / currentPixmap.width();
    }
    zoomFactor /= 1.25;
    if (zoomFactor < 0.1) zoomFactor = 0.1;  // Min 10% zoom
    updateImageDisplay();
}

void MainWindow::zoomReset() {
    zoomFactor = 0.0;  // Back to fit-to-window mode
    updateImageDisplay();
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

    // Restore splitter sizes (terminal panel height)
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

