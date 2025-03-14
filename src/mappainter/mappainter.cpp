/*****************************************************************************
* Copyright 2015-2020 Alexander Barthel alex@littlenavmap.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "mappainter/mappainter.h"

#include "mapgui/mapscale.h"
#include "navapp.h"
#include "common/symbolpainter.h"
#include "geo/calculations.h"
#include "mapgui/mapwidget.h"
#include "common/mapcolors.h"
#include "common/maptypes.h"
#include "mapgui/maplayer.h"
#include "common/aircrafttrack.h"
#include "common/formatter.h"
#include "util/paintercontextsaver.h"
#include "common/unit.h"

#include <marble/GeoDataLineString.h>
#include <marble/GeoPainter.h>

#include <QPixmapCache>
#include <QPainterPath>

using namespace Marble;
using namespace atools::geo;
using atools::roundToInt;

PaintAirportType::PaintAirportType(const map::MapAirport& ap, float x, float y)
  : airport(new map::MapAirport(ap)), point(x, y)
{

}

PaintAirportType::~PaintAirportType()
{
  delete airport;
}

PaintAirportType& PaintAirportType::operator=(const PaintAirportType& other)
{
  if(airport != nullptr && other.airport != nullptr)
    *airport = *other.airport;
  else if(airport == nullptr && other.airport != nullptr)
    airport = new map::MapAirport(*other.airport);
  else if(airport != nullptr && other.airport == nullptr)
  {
    delete airport;
    airport = nullptr;
  }
  // else both nullptr

  point = other.point;
  return *this;
}

void PaintContext::szFont(float scale) const
{
  mapcolors::scaleFont(painter, scale, &defaultFont);
}

textflags::TextFlags PaintContext::airportTextFlags() const
{
  // Build and draw airport text
  textflags::TextFlags textflags = textflags::NONE;

  if(mapLayer->isAirportInfo())
    textflags = textflags::IDENT | textflags::NAME | textflags::INFO;

  if(mapLayer->isAirportIdent())
    textflags |= textflags::IDENT;
  else if(mapLayer->isAirportName())
    textflags |= textflags::NAME;

  if(!(flags2 & opts2::MAP_AIRPORT_TEXT_BACKGROUND))
    textflags |= textflags::NO_BACKGROUND;

  return textflags;
}

textflags::TextFlags PaintContext::airportTextFlagsRoute(bool drawAsRoute, bool drawAsLog) const
{
  // Show ident always on route
  textflags::TextFlags textflags = textflags::IDENT;

  if(drawAsRoute)
    textflags |= textflags::ROUTE_TEXT;

  if(drawAsLog)
    textflags |= textflags::LOG_TEXT;

  // Use more more detailed text for flight plan
  if(mapLayer->isAirportRouteInfo())
    textflags |= textflags::NAME | textflags::INFO;

  if(!(flags2 & opts2::MAP_ROUTE_TEXT_BACKGROUND))
    textflags |= textflags::NO_BACKGROUND;

  return textflags;
}

// =================================================
MapPainter::MapPainter(MapPaintWidget *parentMapWidget, MapScale *mapScale, PaintContext *paintContext)
  : CoordinateConverter(parentMapWidget->viewport()), context(paintContext), mapPaintWidget(parentMapWidget),
  scale(mapScale)
{
  mapQuery = NavApp::getMapQuery();
  airwayQuery = NavApp::getAirwayTrackQuery();
  waypointQuery = NavApp::getWaypointTrackQuery();
  airportQuery = NavApp::getAirportQuerySim();
  symbolPainter = new SymbolPainter();
}

MapPainter::~MapPainter()
{
  delete symbolPainter;
}

bool MapPainter::wToSBuf(const Pos& coords, int& x, int& y, QSize size, const QMargins& margins,
                         bool *hidden) const
{
  float xf, yf;
  bool visible = wToSBuf(coords, xf, yf, size, margins, hidden);
  x = atools::roundToInt(xf);
  y = atools::roundToInt(yf);

  return visible;
}

bool MapPainter::wToSBuf(const Pos& coords, float& x, float& y, QSize size, const QMargins& margins,
                         bool *hidden) const
{
  bool hid = false;
  bool visible = wToS(coords, x, y, size, &hid);

  if(hidden != nullptr)
    *hidden = hid;

  if(!visible && !hid)
    // Check additional visibility using the extended rectangle only if the object is not hidden behind the globe
    return context->screenRect.marginsAdded(margins).contains(atools::roundToInt(x), atools::roundToInt(y));

  return visible;
}

void MapPainter::paintCircle(GeoPainter *painter, const Pos& centerPos, float radiusNm, bool fast,
                             int& xtext, int& ytext)
{
  QRect vpRect(painter->viewport());

  // Calculate the number of points to use depending on screen resolution
  int pixel = scale->getPixelIntForMeter(nmToMeter(radiusNm));
  int numPoints = std::min(std::max(pixel / (fast ? 20 : 2), CIRCLE_MIN_POINTS), CIRCLE_MAX_POINTS);

  float radiusMeter = nmToMeter(radiusNm);

  int step = 360 / numPoints;
  int x1, y1, x2 = -1, y2 = -1;
  xtext = -1;
  ytext = -1;

  QVector<int> xtexts;
  QVector<int> ytexts;

  // Use north endpoint of radius as start position
  Pos startPoint = centerPos.endpoint(radiusMeter, 0);
  Pos p1 = startPoint;
  bool hidden1 = true, hidden2 = true;
  bool visible1 = wToS(p1, x1, y1, DEFAULT_WTOS_SIZE, &hidden1);

  bool ringVisible = false, lastVisible = false;
  LineString ellipse;
  // Draw ring segments and collect potential text positions
  for(int i = step; i <= 360; i += step)
  {
    // Line segment from p1 to p2
    Pos p2 = centerPos.endpoint(radiusMeter, i);

    bool visible2 = wToS(p2, x2, y2, DEFAULT_WTOS_SIZE, &hidden2);

    QRect rect(QPoint(x1, y1), QPoint(x2, y2));
    rect = rect.normalized();
    // Avoid points or flat rectangles (lines)
    rect.adjust(-1, -1, 1, 1);

    // Current line is visible (most likely)
    bool nowVisible = rect.intersects(vpRect);

    if(lastVisible || nowVisible)
      // Last line or this one are visible add coords
      ellipse.append(p1);

    if(lastVisible && !nowVisible)
    {
      // Not visible anymore draw previous line segment
      drawLineString(painter, ellipse);
      ellipse.clear();
    }

    if(lastVisible || nowVisible)
    {
      // At least one segment of the ring is visible
      ringVisible = true;

      if((visible1 || visible2) && !hidden1 && !hidden2)
      {
        if(visible1 && visible2)
        {
          // Remember visible positions for the text (center of the line segment)
          xtexts.append((x1 + x2) / 2);
          ytexts.append((y1 + y2) / 2);
        }
      }
    }
    x1 = x2;
    y1 = y2;
    visible1 = visible2;
    hidden1 = hidden2;
    p1 = p2;
    lastVisible = nowVisible;
  }

  if(ringVisible)
  {
    if(!ellipse.isEmpty())
    {
      // Last one always needs closing the circle
      ellipse.append(startPoint);
      drawLineString(painter, ellipse);
    }

    if(!xtexts.isEmpty() && !ytexts.isEmpty())
    {
      // Take the position at one third of the visible text points to avoid half hidden texts
      xtext = xtexts.at(xtexts.size() / 3);
      ytext = ytexts.at(ytexts.size() / 3);
    }
    else
    {
      xtext = -1;
      ytext = -1;
    }
  }
}

void MapPainter::drawLineStraight(Marble::GeoPainter *painter, const atools::geo::Line& line)
{
  double x1, y1, x2, y2;
  bool visible1 = wToS(line.getPos1(), x1, y1);
  bool visible2 = wToS(line.getPos2(), x2, y2);

  if(visible1 || visible2)
    drawLine(painter, QPointF(x1, y1), QPointF(x2, y2));
}

void MapPainter::drawLine(QPainter *painter, const QLineF& line)
{
  QRectF rect(line.p1(), line.p2());
  // Add margins to avoid null width and height which will not intersect with viewport
  rect = rect.normalized().marginsAdded(QMarginsF(1., 1., 1., 1.));

  if(atools::geo::lineValid(line) && QRectF(painter->viewport()).intersects(rect))
  {
    // if(line.intersect(QLineF(rect.topLeft(), rect.topRight()), nullptr) == QLineF::BoundedIntersection ||
    // line.intersect(QLineF(rect.topRight(), rect.bottomRight()), nullptr) == QLineF::BoundedIntersection ||
    // line.intersect(QLineF(rect.bottomRight(), rect.bottomLeft()), nullptr) == QLineF::BoundedIntersection ||
    // line.intersect(QLineF(rect.bottomLeft(), rect.topLeft()), nullptr) == QLineF::BoundedIntersection)
    painter->drawLine(line);
  }
}

void MapPainter::drawCircle(Marble::GeoPainter *painter, const atools::geo::Pos& center, int radius)
{
  QPoint pt = wToS(center);
  if(!pt.isNull())
    painter->drawEllipse(pt, radius, radius);
}

void MapPainter::drawText(Marble::GeoPainter *painter, const Pos& pos, const QString& text, bool topCorner,
                          bool leftCorner)
{
  QPoint pt = wToS(pos);
  if(!pt.isNull())
  {
    QFontMetrics metrics = painter->fontMetrics();
    pt.setX(leftCorner ? pt.x() : pt.x() - metrics.width(text));
    pt.setY(topCorner ? pt.y() + metrics.ascent() : pt.y() - metrics.descent());
    painter->drawText(pt, text);
  }
}

void MapPainter::drawCross(Marble::GeoPainter *painter, int x, int y, int size)
{
  painter->drawLine(x, y - size, x, y + size);
  painter->drawLine(x - size, y, x + size, y);
}

void MapPainter::drawLineString(Marble::GeoPainter *painter, const atools::geo::LineString& linestring)
{
  GeoDataLineString ls;
  ls.setTessellate(true);
  for(int i = 1; i < linestring.size(); i++)
  {
    if(linestring.at(i - 1).almostEqual(linestring.at(i)))
      // Do not draw  duplicates
      continue;

    // Avoid the straight line Marble draws for equal latitudes - needed to force GC path
    qreal correction = 0.;
    if(atools::almostEqual(linestring.at(i - 1).getLatY(), linestring.at(i).getLatY()))
      correction = 0.000001;

    ls << GeoDataCoordinates(linestring.at(i - 1).getLonX(), linestring.at(i - 1).getLatY() - correction, 0, DEG)
       << GeoDataCoordinates(linestring.at(i).getLonX(), linestring.at(i).getLatY() + correction, 0, DEG);
  }

  QVector<GeoDataLineString *> dateLineCorrected = ls.toDateLineCorrected();
  for(GeoDataLineString *corrected : dateLineCorrected)
    painter->drawPolyline(*corrected);
  qDeleteAll(dateLineCorrected);
}

void MapPainter::drawLine(Marble::GeoPainter *painter, const atools::geo::Line& line)
{
  if(line.isValid() && !line.isPoint())
  {
    // Split long lines to work around the buggy visibility check in Marble
    // Do a quick check using Manhattan distance in degree
    if(line.lengthSimple() > 30.f)
    {
      LineString linestring;
      line.interpolatePoints(line.lengthMeter(), 20, linestring);
      linestring.append(line.getPos2());
      drawLineString(painter, linestring);
    }
    else if(line.lengthSimple() > 5.f)
    {
      LineString linestring;
      line.interpolatePoints(line.lengthMeter(), 5, linestring);
      linestring.append(line.getPos2());
      drawLineString(painter, linestring);
    }
    else
    {
      // Avoid the straight line Marble draws for equal latitudes - needed to force GC path
      qreal correction = 0.;
      if(atools::almostEqual(line.getPos1().getLatY(), line.getPos2().getLatY()))
        correction = 0.000001;

      GeoDataLineString ls;
      ls.setTessellate(true);
      ls << GeoDataCoordinates(line.getPos1().getLonX(), line.getPos1().getLatY() - correction, 0, DEG)
         << GeoDataCoordinates(line.getPos2().getLonX(), line.getPos2().getLatY() + correction, 0, DEG);

      QVector<GeoDataLineString *> dateLineCorrected = ls.toDateLineCorrected();
      for(GeoDataLineString *corrected : dateLineCorrected)
        painter->drawPolyline(*corrected);
      qDeleteAll(dateLineCorrected);
    }
  }
}

void MapPainter::paintArc(QPainter *painter, const QPointF& p1, const QPointF& p2, const QPointF& center, bool left)
{
  QRectF arcRect;
  float startAngle, spanAngle;
  atools::geo::arcFromPoints(QLineF(p1, p2), center, left, &arcRect, &startAngle, &spanAngle);

  painter->drawArc(arcRect, atools::roundToInt(-startAngle * 16.), atools::roundToInt(spanAngle * 16.));
}

void MapPainter::paintHoldWithText(QPainter *painter, float x, float y, float direction,
                                   float lengthNm, float minutes, bool left,
                                   const QString& text, const QString& text2,
                                   const QColor& textColor, const QColor& textColorBackground,
                                   QVector<float> inboundArrows, QVector<float> outboundArrows)
{
  // Scale to total length given in the leg
  // length = 2 * p + 2 * PI * p / 2
  // p = length / (2 + PI)
  // Straight segments are segmentLength long and circle diameter is pixel/2
  // Minimum 3.5

  float segmentLength;
  if(minutes > 0.f)
    // 3.5 nm per minute
    segmentLength = minutes * 3.5f;
  else if(lengthNm > 0.f)
    segmentLength = lengthNm;
  else
    segmentLength = 3.5f;

  float pixel = scale->getPixelForNm(segmentLength);

  // Build the rectangles that are used to draw the arcs ====================
  QRectF arc1, arc2;
  float angle1, span1, angle2, span2;
  QPainterPath path;
  if(!left)
  {
    // Turn right in the hold
    arc1 = QRectF(0, -pixel * 0.25f, pixel * 0.5f, pixel * 0.5f);
    angle1 = 180.f;
    span1 = -180.f;
    arc2 = QRectF(0, 0 + pixel * 0.75f, pixel * 0.5f, pixel * 0.5f);
    angle2 = 0.f;
    span2 = -180.f;
  }
  else
  {
    // Turn left in the hold
    arc1 = QRectF(-pixel * 0.5f, -pixel * 0.25f, pixel * 0.5f, pixel * 0.5f);
    angle1 = 0.f;
    span1 = 180.f;
    arc2 = QRectF(-pixel * 0.5f, 0.f + pixel * 0.75f, pixel * 0.5f, pixel * 0.5f);
    angle2 = 180.f;
    span2 = 180.f;
  }

  path.arcTo(arc1, angle1, span1);
  path.arcTo(arc2, angle2, span2);
  path.closeSubpath();

  // Draw hold ============================================================
  // translate to orgin of hold (navaid or waypoint) and rotate
  painter->translate(x, y);
  painter->rotate(direction);

  // Draw hold
  painter->setBrush(Qt::transparent);
  painter->drawPath(path);

  // Draw arrows if requested ==================================================
  if(!inboundArrows.isEmpty() || !outboundArrows.isEmpty())
  {
    painter->save();
    // Calculate arrow size
    float arrowSize = static_cast<float>(painter->pen().widthF() * 2.3);

    // Use a lighter brush for fill and a thinner pen for lines
    painter->setBrush(painter->pen().color().lighter(300));
    painter->setPen(QPen(painter->pen().color(), painter->pen().widthF() * 0.66));

    if(!inboundArrows.isEmpty())
    {
      QPolygonF arrow = buildArrow(static_cast<float>(arrowSize));
      QLineF inboundLeg(0., pixel, 0., 0.);

      // 0,0 = origin and 0,pixel = start of inbound
      // Drawn an arrow for each position
      for(float pos : inboundArrows)
        painter->drawPolygon(arrow.translated(inboundLeg.pointAt(pos)));
    }

    if(!outboundArrows.isEmpty())
    {
      // Mirror y axis for left turn legs - convert arrow pointing up to pointing  down
      float leftScale = left ? -1.f : 1.f;
      QPolygonF arrowMirror = buildArrow(arrowSize, true);
      QLineF outboundLeg(pixel * 0.5f * leftScale, 0., pixel * 0.5f * leftScale, pixel);

      // 0,0 = origin and 0,pixel = start of inbound
      // Drawn an arrow for each position
      for(float pos : outboundArrows)
        painter->drawPolygon(arrowMirror.translated(outboundLeg.pointAt(pos)));
    }
    painter->restore();
  }

  if(!text.isEmpty() || !text2.isEmpty())
  {
    float lineWidth = static_cast<float>(painter->pen().widthF());
    // Move to first text position
    painter->translate(0, pixel / 2);
    painter->rotate(direction < 180.f ? 270. : 90.);

    painter->save();
    painter->setPen(textColor);
    painter->setBrush(textColorBackground);
    painter->setBackgroundMode(Qt::OpaqueMode);
    painter->setBackground(textColorBackground);

    QFontMetrics metrics = painter->fontMetrics();
    if(!text.isEmpty())
    {
      // text pointing to origin
      QString str = metrics.elidedText(text, Qt::ElideRight, roundToInt(pixel));
      int w1 = metrics.width(str);
      painter->drawText(-w1 / 2, roundToInt(-lineWidth - 3), str);
    }

    if(!text2.isEmpty())
    {
      // text on other side to origin
      QString str = metrics.elidedText(text2, Qt::ElideRight, roundToInt(pixel));
      int w2 = metrics.width(str);

      if(direction < 180.f)
        painter->translate(0, left ? -pixel / 2 : pixel / 2);
      else
        painter->translate(0, left ? pixel / 2 : -pixel / 2);
      painter->drawText(-w2 / 2, roundToInt(-lineWidth - 3), str);
    }
    painter->restore();
  }
  painter->resetTransform();

}

void MapPainter::paintProcedureTurnWithText(QPainter *painter, float x, float y, float turnHeading, float distanceNm,
                                            bool left, QLineF *extensionLine, const QString& text,
                                            const QColor& textColor, const QColor& textColorBackground)
{
  // One minute = 3.5 nm
  float pixel = scale->getPixelForFeet(atools::roundToInt(atools::geo::nmToFeet(3.f)));

  float course;
  if(left)
    // Turn right and then turn 180 deg left
    course = turnHeading - 45.f;
  else
    // Turn left and then turn 180 deg right
    course = turnHeading + 45.f;

  QLineF extension = QLineF(x, y, x + 400.f, y);
  extension.setAngle(angleToQt(course));
  extension.setLength(scale->getPixelForNm(distanceNm, static_cast<float>(angleFromQt(extension.angle()))));

  if(extensionLine != nullptr)
    // Return course
    *extensionLine = QLineF(extension.p2(), extension.p1());

  // Turn segment
  QLineF turnSegment = QLineF(x, y, x + pixel, y);
  float turnCourse;
  if(left)
    turnCourse = course + 45.f;
  else
    turnCourse = course - 45.f;
  turnSegment.setAngle(angleToQt(turnCourse));

  if(!text.isEmpty())
  {
    float lineWidth = static_cast<float>(painter->pen().widthF());

    painter->save();
    painter->setPen(textColor);
    painter->setBackground(textColorBackground);
    QFontMetrics metrics = painter->fontMetrics();
    QString str = metrics.elidedText(text, Qt::ElideRight, roundToInt(turnSegment.length()));
    int w1 = metrics.width(str);

    painter->translate((turnSegment.x1() + turnSegment.x2()) / 2, (turnSegment.y1() + turnSegment.y2()) / 2);
    painter->rotate(turnCourse < 180.f ? turnCourse - 90.f : turnCourse + 90.f);
    painter->drawText(-w1 / 2, roundToInt(-lineWidth - 3), str);
    painter->resetTransform();
    painter->restore();
  }

  // 180 deg turn arc
  QLineF arc = QLineF(turnSegment.x2(), turnSegment.y2(), turnSegment.x2() + pixel / 2., turnSegment.y2());
  if(left)
    arc.setAngle(angleToQt(course - 45.f));
  else
    arc.setAngle(angleToQt(course + 45.f));

  // Return from turn arc
  QLineF returnSegment(turnSegment);
  returnSegment.setP1(arc.p2());
  returnSegment.setP2(QPointF(turnSegment.x1() - (arc.x1() - arc.x2()), turnSegment.y1() - (arc.y1() - arc.y2())));

  // Calculate intersection with extension to get the end point
  QPointF intersect;
  bool intersects = extension.intersect(returnSegment, &intersect) != QLineF::NoIntersection;
  if(intersects)
    returnSegment.setP2(intersect);
  // Make return segment a bit shorter than turn segment
  returnSegment.setLength(returnSegment.length() * 0.8);

  painter->drawLine(turnSegment);
  paintArc(painter, arc.p1(), arc.p2(), arc.pointAt(0.5), left);
  painter->drawLine(returnSegment.p1(), returnSegment.p2());

  // Calculate arrow for return segment
  QLineF arrow(returnSegment.p2(), returnSegment.p1());
  arrow.setLength(scale->getPixelForNm(0.15f, static_cast<float>(angleFromQt(returnSegment.angle()))));

  QPolygonF poly;
  poly << arrow.p2() << arrow.p1();
  if(left)
    arrow.setAngle(angleToQt(turnCourse - 15.f));
  else
    arrow.setAngle(angleToQt(turnCourse + 15.f));
  poly << arrow.p2();

  painter->save();
  QPen pen = painter->pen();
  pen.setCapStyle(Qt::SquareCap);
  pen.setJoinStyle(Qt::MiterJoin);
  painter->setPen(pen);
  painter->drawPolygon(poly);
  painter->restore();
}

QPolygonF MapPainter::buildArrow(float size, bool downwards)
{
  if(downwards)
  {
    // Pointing downwards
    return QPolygonF({QPointF(0., size),
                      QPointF(size, -size),
                      QPointF(0., -size / 2.),
                      QPointF(-size, -size)});
  }
  else
  {
    // Point up
    return QPolygonF({QPointF(0., -size),
                      QPointF(size, size),
                      QPointF(0., size / 2.),
                      QPointF(-size, size)});
  }
}

void MapPainter::paintArrowAlongLine(QPainter *painter, const atools::geo::Line& line, const QPolygonF& arrow,
                                     float pos, float minLengthPx)
{
  bool visible, hidden;
  QPointF pt = wToSF(line.interpolate(pos), DEFAULT_WTOS_SIZE, &visible, &hidden);

  if(visible && !hidden)
  {
    bool draw = true;
    if(minLengthPx > 0.f)
    {
      QLineF lineF;
      wToS(line, lineF, DEFAULT_WTOS_SIZE, &hidden);

      draw = !hidden && lineF.length() > minLengthPx;
    }

    if(draw)
    {
      painter->translate(pt);
      painter->rotate(atools::geo::opposedCourseDeg(line.angleDeg()));
      painter->drawPolygon(arrow);
      painter->resetTransform();
    }
  }
}

void MapPainter::paintArrowAlongLine(QPainter *painter, const QLineF& line, const QPolygonF& arrow, float pos)
{
  painter->translate(line.pointAt(pos));
  painter->rotate(atools::geo::angleFromQt(line.angle()));
  painter->drawPolygon(arrow);
  painter->resetTransform();
}

bool MapPainter::sortAirportFunction(const PaintAirportType& pap1, const PaintAirportType& pap2)
{
  const OptionData& od = OptionData::instance();

  // returns ​true if the first argument is less than (i.e. is ordered before) the second.
  // ">" puts true behind
  const map::MapAirport *ap1 = pap1.airport, *ap2 = pap2.airport;
  bool addon = context->objectTypes.testFlag(map::AIRPORT_ADDON);

  if(!addon || ap1->addon() == ap2->addon()) // no force addon or both are equal - look at more attributes
  {
    if(ap1->emptyDraw(od) == ap2->emptyDraw(od)) // Draw empty on bottom
    {
      if(ap1->waterOnly() == ap2->waterOnly()) // Then water
      {
        if(ap1->helipadOnly() == ap2->helipadOnly()) // Then heliports
        {
          if(ap1->softOnly() == ap2->softOnly()) // Soft airports
          {
            if(ap1->longestRunwayLength == ap2->longestRunwayLength)
              // Use id to get a fixed order and avoid flickering
              return ap1->id < ap2->id;
            else
              return ap1->longestRunwayLength < ap2->longestRunwayLength;
            // Larger value to end of list - drawn on top
          }
          else
            return ap1->softOnly() > ap2->softOnly(); // Larger value to top of list - drawn below all
        }
        else
          return ap1->helipadOnly() > ap2->helipadOnly();
      }
      else
        return ap1->waterOnly() > ap2->waterOnly();
    }
    else
      return ap1->emptyDraw(od) > ap2->emptyDraw(od);
  }
  else
    // Put addon in front
    return ap1->addon() < ap2->addon();
}

void MapPainter::getPixmap(QPixmap& pixmap, const QString& resource, int size)
{
  QPixmap *pixmapPtr = QPixmapCache::find(resource + "_" + QString::number(size));
  if(pixmapPtr == nullptr)
  {
    pixmap = QIcon(resource).pixmap(QSize(size, size));
    QPixmapCache::insert(pixmap);
  }
  else
    pixmap = *pixmapPtr;
}

void MapPainter::paintHoldings(const QList<map::MapHolding>& holdings, bool enroute, bool drawFast)
{
  if(holdings.isEmpty())
    return;

  atools::util::PainterContextSaver saver(context->painter);
  GeoPainter *painter = context->painter;

  bool detail = context->mapLayer->isHoldingInfo();
  bool detail2 = context->mapLayer->isHoldingInfo2();

  QColor backColor = !enroute || context->flags2 &
                     opts2::MAP_NAVAID_TEXT_BACKGROUND ? QColor(Qt::white) : QColor(Qt::transparent);

  if(enroute)
    context->szFont(context->textSizeNavaid);
  else
    context->szFont(context->textSizeRangeDistance);

  for(const map::MapHolding& holding : holdings)
  {
    bool visible, hidden;
    QPointF pt = wToS(holding.getPosition(), DEFAULT_WTOS_SIZE, &visible, &hidden);
    if(hidden)
      continue;

    QColor color = enroute ? mapcolors::holdingColor : holding.color;

    float dist = holding.distance();
    float distPixel = scale->getPixelForNm(dist);
    float lineWidth = enroute ? (detail2 ? 2.5f : 1.5f) : context->szF(context->thicknessRangeDistance, 3);

    if(context->mapLayer->isApproach() && distPixel > 10.f)
    {
      // Calculcate approximate rectangle
      Rect rect(holding.position, atools::geo::nmToMeter(dist) * 2.f);

      if(context->viewportRect.overlaps(rect))
      {
        painter->setPen(QPen(color, lineWidth, Qt::SolidLine));

        QStringList inboundText, outboundText;
        if(detail && !drawFast)
        {
          if(detail2)
          {
            // Text for inbound leg =======================================
            inboundText.append(formatter::courseTextFromTrue(holding.courseTrue, holding.magvar,
                                                             false /* magBold */, false /* trueSmall */,
                                                             true /* narrow */));

            if(holding.time > 0.f)
              inboundText.append(tr("%1min").arg(QString::number(holding.time, 'g', 2)));
            if(holding.length > 0.f)
              inboundText.append(Unit::distNm(holding.length, true /* addUnit */, 1, true /* narrow */));
          }

          if(!holding.navIdent.isEmpty())
            inboundText += holding.navIdent;

          if(detail2)
          {
            // Text for outbound leg =======================================
            outboundText.append(formatter::courseTextFromTrue(opposedCourseDeg(holding.courseTrue), holding.magvar,
                                                              false /* magBold */, false /* trueSmall */,
                                                              true /* narrow */));

            if(!enroute)
            {
              if(holding.speedKts > 0.f)
                outboundText.append(Unit::speedKts(holding.speedKts, true /* addUnit */, true /* narrow */));
              outboundText.append(Unit::altFeet(holding.position.getAltitude(), true /* addUnit */, true /* narrow */));
            }
            else
            {
              if(holding.speedLimit > 0.f)
                outboundText.append(Unit::speedKts(holding.speedLimit, true /* addUnit */, true /* narrow */));

              if(holding.minAltititude > 0.f)
                outboundText.append(tr("A%1").arg(Unit::altFeet(holding.minAltititude, true /* addUnit */,
                                                                true /* narrow */)));
              if(holding.maxAltititude > 0.f)
                outboundText.append(tr("B%2").arg(Unit::altFeet(holding.maxAltititude, true /* addUnit */,
                                                                true /* narrow */)));
            }
          }
        }

        paintHoldWithText(context->painter, static_cast<float>(pt.x()), static_cast<float>(pt.y()),
                          holding.courseTrue, dist, 0.f, holding.turnLeft,
                          inboundText.join(tr("/")), outboundText.join(tr("/")), color, backColor,
                          detail && !drawFast ? QVector<float>({0.80f}) : QVector<float>() /* inbound arrows */,
                          detail && !drawFast ? QVector<float>({0.80f}) : QVector<float>() /* outbound arrows */);
      } // if(context->viewportRect.overlaps(rect))
    } // if(context->mapLayer->isApproach() && scale->getPixelForNm(hold.distance()) > 10.f)

    if(visible /* && (detail || !enroute)*/)
    {
      // Draw triangle at hold fix - independent of zoom factor
      float radius = lineWidth * 2.5f;
      painter->setPen(QPen(color, lineWidth));
      painter->setBrush(backColor);
      painter->drawConvexPolygon(QPolygonF({QPointF(pt.x(), pt.y() - radius),
                                            QPointF(pt.x() + radius / 1.4, pt.y() + radius / 1.4),
                                            QPointF(pt.x() - radius / 1.4, pt.y() + radius / 1.4)}));
    }
  } // for(const map::Hold& hold : holds)
}
