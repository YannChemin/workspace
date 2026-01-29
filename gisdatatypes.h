#ifndef GISDATATYPES_H
#define GISDATATYPES_H

#include <QString>
#include <vector>
#include <cmath>

// Structure to hold selected RGB band indices (1-based as per GDAL)
struct RGBBands {
    int red = 1;
    int green = 2;
    int blue = 3;
    bool fromWavelength = false;  // true if selected based on wavelength metadata
};

// Structure to hold stretch parameters and validation results
struct StretchParams {
    double minP = 0.0;
    double maxP = 1.0;
    bool valid = false;
    QString warning;
};

// Structure to hold full band statistics from .aux.xml
struct BandStatistics {
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double stddev = 0.0;
    bool valid = false;
};

// Structure to hold NODATA value info
struct NoDataInfo {
    double value = 0.0;
    bool hasNoData = false;
};

// Structure to hold GeoTIFF geographic info
struct GeoTIFFInfo {
    double minLon = 0.0, maxLon = 0.0;
    double minLat = 0.0, maxLat = 0.0;  // WGS84 bounds
    bool hasValidCRS = false;
    int pixelWidth = 0;
    int pixelHeight = 0;
    double geoTransform[6] = {0, 1, 0, 0, 0, -1};
};

// Structure to hold GeoJSON geographic info
struct GeoJSONInfo {
    double minLon = 180.0, maxLon = -180.0;
    double minLat = 90.0, maxLat = -90.0;  // WGS84 bounds
    bool hasValidBounds = false;
};

// GeoTransform helper class for coordinate conversions
class GeoTransform {
public:
    GeoTransform() {
        m_gt[0] = 0; m_gt[1] = 1; m_gt[2] = 0;
        m_gt[3] = 0; m_gt[4] = 0; m_gt[5] = -1;
    }

    explicit GeoTransform(const double* gt) {
        for (int i = 0; i < 6; ++i) m_gt[i] = gt[i];
    }

    void set(const double* gt) {
        for (int i = 0; i < 6; ++i) m_gt[i] = gt[i];
    }

    const double* data() const { return m_gt; }

    // Pixel (col, row) to geo coordinates
    void pixelToGeo(double col, double row, double &geoX, double &geoY) const {
        geoX = m_gt[0] + col * m_gt[1] + row * m_gt[2];
        geoY = m_gt[3] + col * m_gt[4] + row * m_gt[5];
    }

    // Geo coordinates to pixel (col, row)
    void geoToPixel(double geoX, double geoY, double &col, double &row) const {
        // Inverse geotransform calculation
        double det = m_gt[1] * m_gt[5] - m_gt[2] * m_gt[4];
        if (std::abs(det) < 1e-10) {
            col = row = 0;
            return;
        }
        double dx = geoX - m_gt[0];
        double dy = geoY - m_gt[3];
        col = (m_gt[5] * dx - m_gt[2] * dy) / det;
        row = (-m_gt[4] * dx + m_gt[1] * dy) / det;
    }

    double pixelWidth() const { return std::abs(m_gt[1]); }
    double pixelHeight() const { return std::abs(m_gt[5]); }

private:
    double m_gt[6];
};

// Target wavelengths for RGB (in nanometers)
constexpr double TARGET_RED_NM = 665.0;    // Red ~650-700nm
constexpr double TARGET_GREEN_NM = 560.0;  // Green ~520-560nm
constexpr double TARGET_BLUE_NM = 470.0;   // Blue ~450-490nm
constexpr double VISIBLE_MIN_NM = 380.0;
constexpr double VISIBLE_MAX_NM = 780.0;

// OSM tile size in pixels
constexpr int OSM_TILE_SIZE = 256;

// Maximum size for GeoJSON rendering
constexpr int GEOJSON_MAX_RENDER_SIZE = 5000;

#endif // GISDATATYPES_H
