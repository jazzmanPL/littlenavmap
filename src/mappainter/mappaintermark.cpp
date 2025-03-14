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

#include "mappainter/mappaintermark.h"

#include "mapgui/mapwidget.h"
#include "navapp.h"
#include "mapgui/mapscale.h"
#include "mapgui/maplayer.h"
#include "common/mapcolors.h"
#include "common/formatter.h"
#include "geo/calculations.h"
#include "util/roundedpolygon.h"
#include "common/symbolpainter.h"
#include "geo/rect.h"
#include "atools.h"
#include "navapp.h"
#include "common/symbolpainter.h"
#include "airspace/airspacecontroller.h"
#include "common/constants.h"
#include "common/unit.h"
#include "route/routeleg.h"
#include "route/route.h"
#include "route/routecontroller.h"
#include "util/paintercontextsaver.h"
#include "common/textplacement.h"
#include "mapgui/mapmarkhandler.h"
#include "fs/userdata/logdatamanager.h"
#include "mapgui/mapscreenindex.h"

#include <marble/GeoDataLineString.h>
#include <marble/GeoDataLinearRing.h>
#include <marble/GeoPainter.h>

#include <QPainterPath>

const float MAX_COMPASS_ROSE_RADIUS_NM = 500.f;
const float MIN_COMPASS_ROSE_RADIUS_NM = 2.f;
const double MIN_VIEW_DISTANCE_COMPASS_ROSE_KM = 6400.;

using namespace Marble;
using namespace atools::geo;
using namespace map;

MapPainterMark::MapPainterMark(MapPaintWidget *mapWidgetParam, MapScale *mapScale, PaintContext *paintContext)
  : MapPainter(mapWidgetParam, mapScale, paintContext)
{
}

MapPainterMark::~MapPainterMark()
{

}

void MapPainterMark::render()
{
  atools::util::PainterContextSaver saver(context->painter);

  map::MapMarkTypes types = NavApp::getMapMarkHandler()->getMarkTypes();

#ifdef DEBUG_INFORMATION
  {
    atools::util::PainterContextSaver saver(context->painter);
    symbolPainter->textBox(context->painter, {
      QString("Layer %1").arg(context->mapLayer->getMaxRange()),
      QString("Airport sym %1").arg(context->mapLayer->getAirportSymbolSize()),
      QString("Min runway %1").arg(context->mapLayer->getMinRunwayLength())},
                           mapcolors::routeLogEntryOutlineColor, 0, 50, textatt::ROUTE_BG_COLOR);
  }
#endif

  paintMark();
  paintHome();

  if(types & map::MARK_PATTERNS)
    paintTrafficPatterns();

  if(types & map::MARK_HOLDS)
    paintHoldings(mapPaintWidget->getHolds(), false /* enroute */, context->drawFast);

  if(types & map::MARK_RANGE_RINGS)
    paintRangeRings();

  if(types & map::MARK_MEASUREMENT)
    paintDistanceMarkers();

  paintCompassRose();

  paintHighlights();

  paintRouteDrag();
  paintUserpointDrag();

#ifdef DEBUG_AIRWAY_PAINT
  context->painter->setPen(QPen(QColor(0, 0, 255, 50), 10, Qt::SolidLine, Qt::RoundCap));
  MapScreenIndex *screenIndex = mapPaintWidget->getScreenIndex();
  screenIndex->updateAirwayScreenGeometry(mapPaintWidget->getCurrentViewBoundingBox());
  QList<std::pair<int, QLine> > airwayLines = screenIndex->getAirwayLines();
  for(std::pair<int, QLine> pair: airwayLines)
    context->painter->drawLine(pair.second);
#endif
}

/* Draw black yellow cross for search distance marker */
void MapPainterMark::paintMark()
{
  GeoPainter *painter = context->painter;

  int x, y;
  if(wToS(mapPaintWidget->getSearchMarkPos(), x, y))
  {
    context->painter->setPen(mapcolors::searchCenterBackPen);
    drawCross(painter, x, y, context->sz(context->symbolSizeAirport, 10));

    context->painter->setPen(mapcolors::searchCenterFillPen);
    drawCross(painter, x, y, context->sz(context->symbolSizeAirport, 8));
  }
}

/* Paint the center of the home position */
void MapPainterMark::paintHome()
{
  GeoPainter *painter = context->painter;

  int x, y;
  if(wToS(mapPaintWidget->getHomePos(), x, y))
  {
    int size = atools::roundToInt(context->szF(context->textSizeRangeDistance, 24));

    if(x < INVALID_INT / 2 && y < INVALID_INT / 2)
    {
      QPixmap pixmap;
      getPixmap(pixmap, ":/littlenavmap/resources/icons/homemap.svg", size);
      painter->drawPixmap(QPoint(x - size / 2, y - size / 2), pixmap);
    }
  }
}

/* Draw rings around objects that are selected on the search or flight plan tables */
void MapPainterMark::paintHighlights()
{
  // Draw highlight from the search result view =====================================================
  const MapResult& highlightResultsSearch = mapPaintWidget->getSearchHighlights();
  int size = context->sz(context->symbolSizeAirport, 6);

  // Get airport entries from log to avoid rings around log entry airports
  QSet<int> logIds;
  for(const MapLogbookEntry& entry : highlightResultsSearch.logbookEntries)
  {
    if(entry.departurePos.isValid())
      logIds.insert(entry.departure.id);
    if(entry.destinationPos.isValid())
      logIds.insert(entry.destination.id);
  }

  QVector<Pos> positions;
  for(const MapAirport& ap : highlightResultsSearch.airports)
  {
    if(!logIds.contains(ap.id))
      positions.append(ap.position);
  }

  for(const MapWaypoint& wp : highlightResultsSearch.waypoints)
    positions.append(wp.position);

  for(const MapVor& vor : highlightResultsSearch.vors)
    positions.append(vor.position);

  for(const MapNdb& ndb : highlightResultsSearch.ndbs)
    positions.append(ndb.position);

  for(const MapUserpoint& user : highlightResultsSearch.userpoints)
    positions.append(user.position);

  for(const map::MapOnlineAircraft& aircraft: highlightResultsSearch.onlineAircraft)
    positions.append(aircraft.getPosition());

  // Draw boundary for selected online network airspaces =====================================================
  for(const MapAirspace& airspace: highlightResultsSearch.airspaces)
  {
    if(airspace.hasValidGeometry())
      paintAirspace(airspace);
  }

  // Draw boundary for airspaces highlighted in the information window =======================================
  for(const MapAirspace& airspace: mapPaintWidget->getAirspaceHighlights())
  {
    if(airspace.hasValidGeometry())
      paintAirspace(airspace);
  }

  // Draw airways highlighted in the information window =====================================================
  for(const QList<MapAirway>& airwayFull : mapPaintWidget->getAirwayHighlights())
    paintAirwayList(airwayFull);
  for(const QList<MapAirway>& airwayFull : mapPaintWidget->getAirwayHighlights())
    paintAirwayTextList(airwayFull);

  // Selected logbook entries ------------------------------------------
  paintLogEntries(highlightResultsSearch.logbookEntries);

  // ====================================================================
  // Draw all highlight rings for positions collected above =============
  GeoPainter *painter = context->painter;
  if(context->mapLayer->isAirport())
    size = context->sz(context->symbolSizeAirport,
                       std::max(size, context->mapLayer->getAirportSymbolSize()));

  QPen outerPen(mapcolors::highlightBackColor, size / 3. + 2., Qt::SolidLine, Qt::FlatCap);
  QPen innerPen(mapcolors::highlightColor, size / 3., Qt::SolidLine, Qt::FlatCap);

  painter->setBrush(Qt::NoBrush);
  painter->setPen(QPen(QBrush(mapcolors::highlightColorFast), size / 3, Qt::SolidLine, Qt::FlatCap));
  for(const Pos& pos : positions)
  {
    int x, y;
    if(wToS(pos, x, y))
    {
      if(!context->drawFast)
      {
        // Draw black background for outline
        painter->setPen(outerPen);
        painter->drawEllipse(QPoint(x, y), size, size);
        painter->setPen(innerPen);
      }
      painter->drawEllipse(QPoint(x, y), size, size);
    }
  }

  // Draw highlights from the approach selection =====================================================
  const proc::MapProcedureLeg& leg = mapPaintWidget->getProcedureLegHighlights();

  if(leg.recFixPos.isValid())
  {
    int ellipseSize = size / 2;

    int x, y;
    if(wToS(leg.recFixPos, x, y))
    {
      // Draw recommended fix with a thin small circle
      if(!context->drawFast)
      {
        painter->setPen(QPen(mapcolors::highlightBackColor, size / 5 + 2));
        painter->drawEllipse(QPoint(x, y), ellipseSize, ellipseSize);
        painter->setPen(QPen(mapcolors::highlightApproachColor, size / 5));
      }
      painter->drawEllipse(QPoint(x, y), ellipseSize, ellipseSize);
    }
  }

  if(leg.line.isValid())
  {
    int ellipseSize = size;

    int x, y;
    if(wToS(leg.line.getPos1(), x, y))
    {
      if(!leg.line.isPoint() || leg.procedureTurnPos.isValid())
        ellipseSize /= 2;

      // Draw start point of the leg using a smaller circle
      if(!context->drawFast)
      {
        painter->setPen(QPen(mapcolors::highlightBackColor, size / 3 + 2));
        painter->drawEllipse(QPoint(x, y), ellipseSize, ellipseSize);
        painter->setPen(QPen(mapcolors::highlightApproachColor, size / 3));
      }
      painter->drawEllipse(QPoint(x, y), ellipseSize, ellipseSize);
    }

    ellipseSize = size;
    if(!leg.line.isPoint())
    {
      if(wToS(leg.line.getPos2(), x, y))
      {
        // Draw end point of the leg using a larger circle
        if(!context->drawFast)
        {
          painter->setPen(QPen(mapcolors::highlightBackColor, size / 3 + 2));
          painter->drawEllipse(QPoint(x, y), ellipseSize, ellipseSize);
          painter->setPen(QPen(mapcolors::highlightApproachColor, size / 3));
        }
        painter->drawEllipse(QPoint(x, y), ellipseSize, ellipseSize);
      }
    }

    if(leg.procedureTurnPos.isValid())
    {
      if(wToS(leg.procedureTurnPos, x, y))
      {
        // Draw turn position of the procedure turn
        if(!context->drawFast)
        {
          painter->setPen(QPen(mapcolors::highlightBackColor, size / 3 + 2));
          painter->drawEllipse(QPoint(x, y), ellipseSize, ellipseSize);
          painter->setPen(QPen(mapcolors::highlightApproachColor, size / 3));
        }
        painter->drawEllipse(QPoint(x, y), ellipseSize, ellipseSize);
      }
    }
  }

  // Draw highlights from the flight plan view =====================================================
  if(context->mapLayer->isAirport())
    size = std::max(size, context->mapLayer->getAirportSymbolSize());

  const QList<int>& routeHighlightResults = mapPaintWidget->getRouteHighlights();
  positions.clear();
  for(int idx : routeHighlightResults)
  {
    const RouteLeg& routeLeg = NavApp::getRouteConst().value(idx);
    positions.append(routeLeg.getPosition());
  }

  painter->setBrush(Qt::NoBrush);
  painter->setPen(QPen(QBrush(mapcolors::routeHighlightColorFast), size / 3, Qt::SolidLine, Qt::FlatCap));
  for(const Pos& pos : positions)
  {
    int x, y;
    if(wToS(pos, x, y))
    {
      if(!context->drawFast)
      {
        painter->setPen(QPen(QBrush(mapcolors::routeHighlightBackColor), size / 3 + 2, Qt::SolidLine,
                             Qt::FlatCap));
        painter->drawEllipse(QPoint(x, y), size, size);
        painter->setPen(QPen(QBrush(mapcolors::routeHighlightColor), size / 3, Qt::SolidLine, Qt::FlatCap));
      }
      painter->drawEllipse(QPoint(x, y), size, size);
    }
  }

  // Draw hightlight from the elevation profile view =====================================================
  painter->setBrush(Qt::NoBrush);
  painter->setPen(QPen(QBrush(mapcolors::profileHighlightColorFast), size / 3, Qt::SolidLine, Qt::FlatCap));
  const Pos& pos = mapPaintWidget->getProfileHighlight();
  if(pos.isValid())
  {
    int x, y;
    if(wToS(pos, x, y))
    {
      if(!context->drawFast)
      {
        painter->setPen(QPen(QBrush(mapcolors::profileHighlightBackColor), size / 3 + 2, Qt::SolidLine,
                             Qt::FlatCap));
        painter->drawEllipse(QPoint(x, y), size, size);
        painter->setPen(QPen(QBrush(mapcolors::profileHighlightColor), size / 3, Qt::SolidLine, Qt::FlatCap));
      }
      painter->drawEllipse(QPoint(x, y), size, size);
    }
  }
}

void MapPainterMark::paintLogEntries(const QList<map::MapLogbookEntry>& entries)
{
  GeoPainter *painter = context->painter;
  painter->setBackgroundMode(Qt::TransparentMode);
  painter->setBackground(mapcolors::routeOutlineColor);
  painter->setBrush(Qt::NoBrush);
  context->szFont(context->textSizeFlightplan);

  // Collect visible feature parts ==========================================================================
  atools::fs::userdata::LogdataManager *logdataManager = NavApp::getLogdataManager();
  QVector<const MapLogbookEntry *> visibleLogEntries, allLogEntries;
  QVector<atools::geo::LineString> visibleRouteGeometries;
  QVector<QStringList> visibleRouteTexts;
  QVector<atools::geo::LineString> visibleTrackGeometries;

  for(const MapLogbookEntry& entry : entries)
  {
    // All selected for airport drawing
    allLogEntries.append(&entry);

    // All which have visible geometry
    if(context->viewportRect.overlaps(entry.bounding()))
      visibleLogEntries.append(&entry);

    // Show details only if one entry is selected
    if(entries.size() == 1)
    {
      const atools::fs::userdata::LogEntryGeometry *geometry = logdataManager->getGeometry(entry.id);
      // Geometry might be null in case of cache overflow
      if(geometry != nullptr)
      {
        // Geometry has to be copied since cache in LogDataManager might remove it any time

        // Limit number of visible routes
        if(context->objectDisplayTypes & map::LOGBOOK_ROUTE)
        {
          if(context->viewportRect.overlaps(geometry->routeRect))
            visibleRouteGeometries.append(geometry->route);
          else
            // Insert null to have it in sync with route texts
            visibleRouteGeometries.append(atools::geo::EMPTY_LINESTRING);

          visibleRouteTexts.append(geometry->names);
        }

        // Limit number of visible tracks
        if(context->objectDisplayTypes & map::LOGBOOK_TRACK && context->viewportRect.overlaps(geometry->trackRect))
        {
          for(const atools::geo::LineString& line : geometry->tracks)
          {
            if(context->viewportRect.overlaps(line.boundingRect()))
              visibleTrackGeometries.append(line);
          }
        }
      }
    }
  }

  // Draw route ==========================================================================
  if(context->objectDisplayTypes & map::LOGBOOK_ROUTE && !visibleRouteGeometries.isEmpty())
  {
    float outerlinewidth = context->sz(context->thicknessFlightplan, 7);
    float innerlinewidth = context->sz(context->thicknessFlightplan, 4);
    float symbolSize = context->sz(context->thicknessFlightplan, 10);

    painter->setPen(QPen(mapcolors::routeLogEntryOutlineColor, outerlinewidth, Qt::SolidLine, Qt::RoundCap,
                         Qt::RoundJoin));

    // Draw outline for all selected entries ===============
    for(const atools::geo::LineString& geo: visibleRouteGeometries)
    {
      if(geo.isValid())
        drawLineString(painter, geo);
    }

    // Draw line for all selected entries ===============
    // Use a lighter pen for the flight plan legs ======================================
    QPen routePen(mapcolors::routeLogEntryColor, innerlinewidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    routePen.setColor(mapcolors::routeLogEntryColor.lighter(130));
    painter->setPen(routePen);

    for(int i = 0; i < visibleRouteGeometries.size(); i++)
    {
      const atools::geo::LineString& geo = visibleRouteGeometries.at(i);

      if(geo.isValid())
      {
        drawLineString(painter, geo);

        // Draw waypoint symbols and text for route preview =========
        const QStringList& names = visibleRouteTexts.at(i);
        for(int j = 1; j < geo.size() - 1; j++)
        {
          float x, y;
          if(wToS(geo.at(j), x, y))
          {
            symbolPainter->drawLogbookPreviewSymbol(context->painter, x, y, symbolSize);

            if(context->mapLayer->isWaypointRouteName() && names.size() == geo.size())
              symbolPainter->textBoxF(context->painter, {names.at(j)}, mapcolors::routeLogEntryOutlineColor,
                                      x + symbolSize / 2 + 2, y, textatt::LOG_BG_COLOR);
          }
        }
      }
    }

    painter->setPen(QPen(mapcolors::routeLogEntryOutlineColor, (outerlinewidth - innerlinewidth) / 2.,
                         Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::white);
    QPolygonF arrow = buildArrow(outerlinewidth);
    for(int i = 0; i < visibleRouteGeometries.size(); i++)
    {
      const atools::geo::LineString& geo = visibleRouteGeometries.at(i);
      if(geo.isValid())
      {
        // Draw waypoint symbols and text for route preview =========
        for(int j = 1; j < geo.size(); j++)
          paintArrowAlongLine(painter, Line(geo.at(j), geo.at(j - 1)), arrow, 0.5f /* pos*/, 40.f /* minLengthPx */);
      }
    }
  }

  // Draw track ==========================================================================
  if(context->objectDisplayTypes & map::LOGBOOK_TRACK && !visibleTrackGeometries.isEmpty())
  {
    // Use a darker pen for the trail but same style as normal trail ======================================
    QPen trackPen = mapcolors::aircraftTrailPen(context->sz(context->thicknessTrail, 2));
    trackPen.setColor(mapcolors::routeLogEntryColor.darker(200));
    painter->setPen(trackPen);

    for(const atools::geo::LineString& geo: visibleTrackGeometries)
    {
      if(geo.isValid())
        drawLineString(painter, geo);
    }
  }

  // Draw direct connection ==========================================================================
  if(context->objectDisplayTypes & map::LOGBOOK_DIRECT)
  {
    // Use smaller measurement line thickness for this direct connection
    float outerlinewidth = context->sz(context->thicknessRangeDistance, 7) * 0.6f;
    float innerlinewidth = context->sz(context->thicknessRangeDistance, 4) * 0.6f;
    QPen directPen(mapcolors::routeLogEntryColor, innerlinewidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen directOutlinePen(mapcolors::routeLogEntryOutlineColor, outerlinewidth, Qt::SolidLine, Qt::RoundCap,
                          Qt::RoundJoin);
    int size = context->sz(context->symbolSizeAirport, context->mapLayer->getAirportSymbolSize());

    QVector<LineString> geo;
    for(const MapLogbookEntry *entry : visibleLogEntries)
      geo.append(entry->lineString());

    // Outline
    int circleSize = size;
    painter->setPen(directOutlinePen);
    for(const LineString& line : geo)
    {
      if(line.size() > 1)
        drawLineString(painter, line);
      else
        drawCircle(painter, line.getPos1(), circleSize);
    }

    // Center line
    painter->setPen(directPen);
    for(const LineString& line : geo)
    {
      if(line.size() > 1)
        drawLineString(painter, line);
      else
        drawCircle(painter, line.getPos1(), circleSize);
    }

    // Draw line text ==========================================================================
    context->szFont(context->textSizeRangeDistance);
    painter->setBackground(mapcolors::routeTextBackgroundColor);
    painter->setPen(mapcolors::routeTextColor);
    for(const MapLogbookEntry *entry : visibleLogEntries)
    {
      // Text for one line
      LineString positions = entry->lineString();

      TextPlacement textPlacement(context->painter, this, QRect());
      textPlacement.setDrawFast(context->drawFast);
      textPlacement.setLineWidth(outerlinewidth);
      textPlacement.calculateTextPositions(positions);

      QStringList text;
      text.append(tr("%1 to %2").arg(entry->departureIdent).arg(entry->destinationIdent));
      // text.append(atools::elideTextShort(entry->aircraftType, 5));
      // text.append(atools::elideTextShort(entry->aircraftRegistration, 7));
      if(entry->distanceGc > 0.f)
        text.append(Unit::distNm(entry->distanceGc, true /* unit */, 20, true /* narrow */));
      text.removeAll(QString());

      if(positions.size() >= 2)
      {
        textPlacement.calculateTextAlongLines({positions.toLine()}, {text.join(tr(","))});
        textPlacement.drawTextAlongLines();
      }
    }
  }

  // Draw airport symbols and text always ==========================================================================
  float x, y;
  textflags::TextFlags flags = context->airportTextFlagsRoute(false /* draw as route */, true /* draw as log */);
  int size = context->sz(context->symbolSizeAirport, context->mapLayer->getAirportSymbolSize());
  context->szFont(context->textSizeFlightplan);
  QMargins margins(120, 10, 10, 10);

  QSet<int> airportIds;
  for(const MapLogbookEntry *entry : allLogEntries)
  {
    // Check if already drawn
    if(!airportIds.contains(entry->departure.id))
    {
      if(entry->departure.isValid() ?
         wToSBuf(entry->departure.position, x, y, margins) : // Use valid airport position
         wToSBuf(entry->departurePos, x, y, margins)) // Use recorded position
      {
        symbolPainter->drawAirportSymbol(context->painter, entry->departure, x, y, size, false, context->drawFast,
                                         context->flags2.testFlag(opts2::MAP_AIRPORT_HIGHLIGHT_ADDON));
        symbolPainter->drawAirportText(context->painter, entry->departure, x, y, context->dispOptsAirport, flags, size,
                                       context->mapLayer->isAirportDiagram(),
                                       context->mapLayer->getMaxTextLengthAirport());
      }
      airportIds.insert(entry->departure.id);
    }

    if(!airportIds.contains(entry->destination.id))
    {
      if(entry->destination.isValid() ?
         wToSBuf(entry->destination.position, x, y, margins) :
         wToSBuf(entry->destinationPos, x, y, margins))
      {
        symbolPainter->drawAirportSymbol(context->painter, entry->destination, x, y, size, false, context->drawFast,
                                         context->flags2.testFlag(opts2::MAP_AIRPORT_HIGHLIGHT_ADDON));
        symbolPainter->drawAirportText(context->painter, entry->destination, x, y, context->dispOptsAirport, flags,
                                       size,
                                       context->mapLayer->isAirportDiagram(),
                                       context->mapLayer->getMaxTextLengthAirport());
      }
      airportIds.insert(entry->destination.id);
    }
  }
}

void MapPainterMark::paintAirwayList(const QList<map::MapAirway>& airwayList)
{
  Marble::GeoPainter *painter = context->painter;

  // Collect all waypoints from airway list ===========================
  LineString linestring;
  if(!airwayList.isEmpty())
    linestring.append(airwayList.first().from);
  for(const map::MapAirway& airway : airwayList)
  {
    if(airway.isValid())
      linestring.append(airway.to);
  }

  // Outline =================
  float lineWidth = context->szF(context->thicknessRangeDistance, 5.f);
  QPen outerPen(mapcolors::highlightBackColor, lineWidth, Qt::SolidLine, Qt::RoundCap);
  painter->setPen(outerPen);
  drawLineString(painter, linestring);

  // Inner line ================
  QPen innerPen(mapcolors::airwayVictorColor, lineWidth);
  innerPen.setWidthF(lineWidth * 0.5);
  innerPen.setColor(innerPen.color().lighter(150));
  painter->setPen(innerPen);
  drawLineString(painter, linestring);

  // Arrows ================
  QPolygonF arrow = buildArrow(lineWidth);
  context->painter->setPen(QPen(mapcolors::highlightBackColor, lineWidth / 3., Qt::SolidLine, Qt::RoundCap));
  context->painter->setBrush(Qt::white);
  for(const map::MapAirway& airway : airwayList)
  {
    if(airway.direction != map::DIR_BOTH)
    {
      Line arrLine = airway.direction !=
                     map::DIR_FORWARD ? Line(airway.from, airway.to) : Line(airway.to, airway.from);
      paintArrowAlongLine(context->painter, arrLine, arrow, 0.3f);
    }
  }

  // Draw waypoint triangles =============================================
  for(const atools::geo::Pos& pos : linestring)
  {
    QPointF pt = wToS(pos);
    if(!pt.isNull())
    {
      // Draw a triangle
      double radius = lineWidth * 0.8;
      QPolygonF polygon;
      polygon << QPointF(pt.x(), pt.y() - (radius * 1.2)) << QPointF(pt.x() + radius, pt.y() + radius)
              << QPointF(pt.x() - radius, pt.y() + radius);

      painter->drawConvexPolygon(polygon);
    }
  }
}

void MapPainterMark::paintAirwayTextList(const QList<map::MapAirway>& airwayList)
{
  context->szFont(context->textSizeRangeDistance);

  for(const map::MapAirway& airway : airwayList)
  {
    if(airway.isValid())
    {
      QPen innerPen = mapcolors::colorForAirwayTrack(airway);

      // Draw text  at center position of a line
      int x, y;
      Pos center = airway.bounding.getCenter();
      bool visible1, hidden1, visible2, hidden2;
      QPoint p1 = wToS(airway.from, DEFAULT_WTOS_SIZE, &visible1, &hidden1);
      QPoint p2 = wToS(airway.to, DEFAULT_WTOS_SIZE, &visible2, &hidden2);

      // Draw if not behind the globe and sufficient distance
      if((p1 - p2).manhattanLength() > 40)
      {
        if(wToS(center, x, y))
        {
          if(!hidden1 && !hidden2)
            symbolPainter->textBoxF(context->painter, {airway.name}, innerPen, x, y, textatt::CENTER);
        }
      }
    }
  }
}

void MapPainterMark::paintAirspace(const map::MapAirspace& airspace)
{
  const LineString *airspaceGeometry = NavApp::getAirspaceController()->getAirspaceGeometry(airspace.combinedId());
  Marble::GeoPainter *painter = context->painter;

  float lineWidth = context->szF(context->thicknessRangeDistance, 5);

  QPen outerPen(mapcolors::highlightBackColor, lineWidth, Qt::SolidLine, Qt::FlatCap);

  // Make boundary pen the same color as airspace boundary without transparency
  QPen innerPen = mapcolors::penForAirspace(airspace);
  innerPen.setWidthF(lineWidth * 0.5);
  QColor c = innerPen.color();
  c.setAlpha(255);
  innerPen.setColor(c);

  painter->setBrush(mapcolors::colorForAirspaceFill(airspace));
  context->szFont(context->textSizeRangeDistance);

  if(airspaceGeometry != nullptr)
  {
    if(context->viewportRect.overlaps(airspace.bounding))
    {
      if(context->objCount())
        return;

      Marble::GeoDataLinearRing linearRing;
      linearRing.setTessellate(true);

      for(const Pos& pos : *airspaceGeometry)
        linearRing.append(Marble::GeoDataCoordinates(pos.getLonX(), pos.getLatY(), 0, DEG));
      GeoDataCoordinates center = linearRing.latLonAltBox().center();

      if(!context->drawFast)
      {
        // Draw black background for outline
        painter->setPen(outerPen);
        painter->drawPolygon(linearRing);
        painter->setPen(innerPen);
      }
      painter->drawPolygon(linearRing);

      int x, y;
      if(wToS(center, x, y))
      {
        QStringList texts;
        texts << (airspace.isOnline() ? airspace.name : formatter::capNavString(airspace.name))
              << map::airspaceTypeToString(airspace.type);
        if(!airspace.restrictiveDesignation.isEmpty())
          texts << (airspace.restrictiveType + "-" + airspace.restrictiveDesignation);

        symbolPainter->textBoxF(painter, {texts}, innerPen, x, y, textatt::CENTER);
      }
    }
  }
}

/* Draw all rang rings. This includes the red rings and the radio navaid ranges. */
void MapPainterMark::paintRangeRings()
{
  atools::util::PainterContextSaver saver(context->painter);
  const QList<map::RangeMarker>& rangeRings = mapPaintWidget->getRangeRings();
  GeoPainter *painter = context->painter;

  painter->setBrush(Qt::NoBrush);
  context->szFont(context->textSizeRangeDistance);

  float lineWidth = context->szF(context->thicknessRangeDistance, 3);

  for(const map::RangeMarker& rings : rangeRings)
  {
    // Get the biggest ring to check visibility
    QVector<float>::const_iterator maxRingIter = std::max_element(rings.ranges.begin(), rings.ranges.end());

    if(maxRingIter != rings.ranges.end())
    {
      float maxRadiusNm = *maxRingIter;

      if(context->viewportRect.overlaps(Rect(rings.position,
                                             nmToMeter(maxRadiusNm))) || maxRadiusNm > 2000.f /*&& !fast*/)
      {
        // Ring is visible - the rest of the visibility check is done in paintCircle

        // Select color according to source
        QColor color = mapcolors::rangeRingColor, textColor = mapcolors::rangeRingTextColor;
        if(rings.type == map::VOR)
        {
          color = mapcolors::vorSymbolColor;
          textColor = mapcolors::vorSymbolColor;
        }
        else if(rings.type == map::NDB)
        {
          color = mapcolors::ndbSymbolColor;
          textColor = mapcolors::ndbSymbolColor;
        }

        painter->setPen(QPen(QBrush(color), lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin));

        bool centerVisible;
        QPointF center = wToSF(rings.position, DEFAULT_WTOS_SIZE, &centerVisible);
        if(centerVisible)
        {
          // Draw small center point
          painter->setBrush(Qt::white);
          painter->drawEllipse(center, 4., 4.);
        }

        // Draw all rings
        for(float radius : rings.ranges)
        {
          int xt, yt;
          paintCircle(painter, rings.position, radius, context->drawFast, xt, yt);
          if(xt != -1 && yt != -1)
          {
            // paintCircle found a text position - draw text
            painter->setPen(textColor);

            QStringList texts;

            if(!rings.text.isEmpty())
              texts.append(rings.text);

            // Build narrow text manually
            if(radius > 0.f)
              texts.append(tr("%1%2").
                           arg(QLocale(QLocale::C).toString(Unit::distNmF(radius), 'g', 6)).
                           arg(Unit::getUnitDistStr()));

            yt += painter->fontMetrics().height() / 2 - painter->fontMetrics().descent();
            symbolPainter->textBox(painter, texts, painter->pen(), xt, yt, textatt::CENTER);

            painter->setPen(QPen(QBrush(color), lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin));
          }
        }
      }
    }
  }
}

/* Draw a compass rose for the user aircraft with tick marks. */
void MapPainterMark::paintCompassRose()
{
  if(context->objectDisplayTypes & map::COMPASS_ROSE && mapPaintWidget->distance() < MIN_VIEW_DISTANCE_COMPASS_ROSE_KM)
  {
    atools::util::PainterContextSaver saver(context->painter);

    Marble::GeoPainter *painter = context->painter;
    const atools::fs::sc::SimConnectUserAircraft& aircraft = mapPaintWidget->getUserAircraft();
    Pos pos = aircraft.getPosition();

    // Use either aircraft position or viewport center
    QRect viewport = painter->viewport();
    bool hasAircraft = pos.isValid() && context->objectDisplayTypes & map::COMPASS_ROSE_ATTACH;
    if(!hasAircraft)
      pos = sToW(viewport.center());

    // Get vertical and horizontal dimensions and calculate size
    Line horiz(context->viewportRect.getLeftCenter(), context->viewportRect.getRightCenter());
    Line vert(context->viewportRect.getTopCenter(), context->viewportRect.getBottomCenter());

    float h = horiz.lengthMeter();
    float v = vert.lengthMeter();
    if(h < 1000.f)
      h = v;

    float radiusMeter = std::min(h, v) / 2.f * 0.8f;

    radiusMeter = std::min(radiusMeter, nmToMeter(MAX_COMPASS_ROSE_RADIUS_NM));
    radiusMeter = std::max(radiusMeter, nmToMeter(MIN_COMPASS_ROSE_RADIUS_NM));

    float radiusNm = meterToNm(radiusMeter);

    painter->setBrush(Qt::NoBrush);
    float lineWidth = context->szF(context->thicknessCompassRose, 2);
    QPen rosePen(QBrush(mapcolors::compassRoseColor), lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
    QPen rosePenSmall(QBrush(mapcolors::compassRoseColor), lineWidth / 4.f, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
    QPen headingLinePen(QBrush(mapcolors::compassRoseColor), lineWidth, Qt::DotLine, Qt::RoundCap,
                        Qt::MiterJoin);
    painter->setPen(rosePen);

    // Draw outer big circle
    int xt, yt;
    paintCircle(context->painter, pos, meterToNm(radiusMeter), context->drawFast, xt, yt);

    // Draw small center circle if no aircraft
    QPointF centerPoint = wToSF(pos);
    if(!centerPoint.isNull() && !hasAircraft)
      painter->drawEllipse(centerPoint, 5, 5);

    // Collect points for tick marks and labels
    float magVar = hasAircraft ? aircraft.getMagVarDeg() : NavApp::getMagVar(pos);
    QVector<Pos> endpoints;
    QVector<QPointF> endpointsScreen;
    for(float angle = 0.f; angle < 360.f; angle += 5)
    {
      endpoints.append(pos.endpoint(radiusMeter, magVar + angle));
      endpointsScreen.append(wToSF(endpoints.last()));
    }

    // Draw tick marks ======================================================
    if(context->dOptRose(optsd::ROSE_DEGREE_MARKS))
    {
      for(int i = 0; i < 360 / 5; i++)
      {
        if((i % (90 / 5)) == 0) // 90 degree ticks
        {
          drawLineStraight(painter, Line(pos.interpolate(endpoints.at(i), radiusMeter, 0.8f), endpoints.at(i)));
        }
        else if((i % (45 / 5)) == 0) // 45 degree ticks
          drawLineStraight(painter, Line(pos.interpolate(endpoints.at(i), radiusMeter, 0.84f), endpoints.at(i)));
        else if((i % (10 / 5)) == 0) // 10 degree ticks
        {
          if(mapPaintWidget->distance() < 3200 /* km */)
            drawLineStraight(painter, Line(pos.interpolate(endpoints.at(i), radiusMeter, 0.92f), endpoints.at(i)));
        }
        else if(mapPaintWidget->distance() < 6400 /* km */) // 5 degree ticks
          drawLineStraight(painter, Line(pos.interpolate(endpoints.at(i), radiusMeter, 0.95f), endpoints.at(i)));
      }
    }

    painter->setBrush(QBrush(Qt::white));
    // Calculate and draw triangle for true north ======================================================
    Pos trueNorth = pos.endpoint(radiusMeter, 0);
    QPointF trueNorthPoint = wToSF(trueNorth);

    if(!trueNorthPoint.isNull())
      painter->drawPolygon(QPolygonF({trueNorthPoint, trueNorthPoint - QPointF(10, 20),
                                      trueNorthPoint - QPointF(-10, 20)}));

    // Aircraft track and heading line ======================================================
    // Convert to selected display unit
    float radiusUnit = Unit::distNmF(radiusNm);
    float stepsizeUnit = atools::calculateSteps(radiusUnit, 6.f);
    float stepsizeNm = Unit::rev(stepsizeUnit, Unit::distNmF);
    painter->setPen(rosePenSmall);

    // Draw distance circles =======================================================
    if(context->dOptRose(optsd::ROSE_RANGE_RINGS))
    {
      for(float i = 1.f; i * stepsizeNm < radiusNm; i++)
        paintCircle(context->painter, pos, i * stepsizeNm, context->drawFast, xt, yt);
    }
    painter->setPen(rosePen);

    if(hasAircraft)
    {
      // Solid track line
      painter->setPen(rosePen);
      if(context->dOptRose(optsd::ROSE_TRACK_LINE))
      {
        float trackTrue = aircraft.getTrackDegTrue();
        Pos trueTrackPos = pos.endpoint(radiusMeter, trackTrue);
        drawLine(painter, Line(pos, trueTrackPos));
      }

      // Dotted heading line
      if(context->dOptRose(optsd::ROSE_HEADING_LINE))
      {
        float headingTrue = aircraft.getHeadingDegTrue();
        Pos trueHeadingPos = pos.endpoint(radiusMeter, headingTrue);
        painter->setPen(headingLinePen);
        drawLine(painter, Line(pos, trueHeadingPos));
      }
      painter->setPen(rosePen);
    }

    // Draw labels for four directions ======================================================
    context->szFont(context->textSizeCompassRose * 1.4f);
    painter->setPen(mapcolors::compassRoseTextColor);

    if(context->dOptRose(optsd::ROSE_DIR_LABLES))
    {
      for(int i = 0; i < 360 / 5; i++)
      {
        if((i % (90 / 5)) == 0)
        {
          if(mapPaintWidget->distance() < 6400 /* km */)
          {
            if(!endpointsScreen.at(i).isNull())
            {
              QString text;
              if(i == 0)
                text = tr("N", "North");
              else if(i == 90 / 5)
                text = tr("E", "East");
              else if(i == 180 / 5)
                text = tr("S", "South");
              else if(i == 270 / 5)
                text = tr("W", "West");

              symbolPainter->textBoxF(painter, {text}, painter->pen(),
                                      static_cast<float>(endpointsScreen.at(i).x()),
                                      static_cast<float>(endpointsScreen.at(i).y()), textatt::CENTER);
            }
          }
        }
      }
    }

    // Draw small 15 deg labels ======================================================
    if(mapPaintWidget->distance() < 1600 /* km */ && context->dOptRose(optsd::ROSE_DEGREE_LABELS))
    {
      // Reduce font size
      context->szFont(context->textSizeCompassRose * 0.8f);
      for(int i = 0; i < 360 / 5; i++)
      {
        if((i % (15 / 5)) == 0 && (!context->dOptRose(optsd::ROSE_DIR_LABLES) || !((i % (90 / 5)) == 0)))
        {
          // All 15 deg but not at 90 deg boundaries
          symbolPainter->textBoxF(painter, {QString::number(i * 5)}, painter->pen(),
                                  static_cast<float>(endpointsScreen.at(i).x()),
                                  static_cast<float>(endpointsScreen.at(i).y()), textatt::CENTER);
        }
      }
    }

    // Aircraft track line end text and distance labels along track line ======================================================
    float trackTrue = 0.f;
    if(hasAircraft)
      // Solid track line
      trackTrue = aircraft.getTrackDegTrue();

    // Distance labels along track line
    context->szFont(context->textSizeCompassRose * 0.8f);
    if(context->dOptRose(optsd::ROSE_RANGE_RINGS))
    {
      for(float i = 1.f; i * stepsizeNm < radiusNm; i++)
      {
        QPointF s = wToSF(pos.endpoint(nmToMeter(i * stepsizeNm), trackTrue));
        if(!s.isNull())
          symbolPainter->textBoxF(painter, {Unit::distNm(i * stepsizeNm, true, 20, true)}, painter->pen(),
                                  static_cast<float>(s.x()), static_cast<float>(s.y()), textatt::CENTER);
      }
    }

    if(hasAircraft)
    {
      const Route& route = NavApp::getRouteConst();

      if(route.getSizeWithoutAlternates() > 1 && aircraft.isFlying())
      {
        bool isCorrected = false;
        int activeLegCorrected = route.getActiveLegIndexCorrected(&isCorrected);
        if(activeLegCorrected != map::INVALID_INDEX_VALUE)
        {
          // Draw crab angle if flight plan is available ========================

          // If approaching an initial fix use corrected version
          int activeLeg = route.getActiveLegIndex();
          const RouteLeg& routeLeg = activeLeg != map::INVALID_INDEX_VALUE && isCorrected ?
                                     route.value(activeLeg) : route.value(activeLegCorrected);

          float courseToWptTrue = map::INVALID_COURSE_VALUE;
          if((routeLeg.isRoute() || !routeLeg.getProcedureLeg().isCircular()) && routeLeg.getPosition().isValid())
            courseToWptTrue = aircraft.getPosition().angleDegTo(routeLeg.getPosition());

          if(context->dOptRose(optsd::ROSE_CRAB_ANGLE) && courseToWptTrue < map::INVALID_COURSE_VALUE)
          {
            // Crab angle is the amount of correction an aircraft must be turned into the wind in order to maintain the desired course.
            float crabAngle = windCorrectedHeading(aircraft.getWindSpeedKts(), aircraft.getWindDirectionDegT(),
                                                   courseToWptTrue, aircraft.getTrueAirspeedKts());

            Pos crabPos = pos.endpoint(radiusMeter, crabAngle);
            painter->setPen(rosePen);
            painter->setBrush(OptionData::instance().getFlightplanActiveSegmentColor());

            QPointF crabScreenPos = wToSF(crabPos);
            painter->drawEllipse(crabScreenPos, lineWidth * 3, lineWidth * 3);
          }

          if(context->dOptRose(optsd::ROSE_NEXT_WAYPOINT) && courseToWptTrue < map::INVALID_COURSE_VALUE)
          {
            // Draw small line to show course to next waypoint ========================
            Pos endPt = pos.endpoint(radiusMeter, courseToWptTrue);
            Line crsLine(pos.interpolate(endPt, radiusMeter, 0.92f), endPt);
            painter->setPen(QPen(mapcolors::routeOutlineColor, context->sz(context->thicknessFlightplan, 7),
                                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            drawLineStraight(painter, crsLine);

            painter->setPen(QPen(OptionData::instance().getFlightplanActiveSegmentColor(),
                                 context->sz(context->thicknessFlightplan, 4),
                                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            drawLineStraight(painter, crsLine);
          }
        }
      }

      // Aircraft track label at end of track line ======================================================
      if(context->dOptRose(optsd::ROSE_TRACK_LABEL))
      {
        QPointF trueTrackTextPoint = wToSF(pos.endpoint(radiusMeter * 1.1f, trackTrue));
        if(!trueTrackTextPoint.isNull())
        {
          painter->setPen(mapcolors::compassRoseTextColor);
          context->szFont(context->textSizeCompassRose);
          QString text =
            tr("%1°M").arg(QString::number(atools::roundToInt(aircraft.getTrackDegMag())));
          symbolPainter->textBoxF(painter, {text, tr("TRK")}, painter->pen(),
                                  static_cast<float>(trueTrackTextPoint.x()),
                                  static_cast<float>(trueTrackTextPoint.y()),
                                  textatt::CENTER | textatt::ROUTE_BG_COLOR);
        }
      }
    }
  }
}

/* Draw great circle line distance measurement lines */
void MapPainterMark::paintDistanceMarkers()
{
  atools::util::PainterContextSaver saver(context->painter);
  GeoPainter *painter = context->painter;
  context->szFont(context->textSizeRangeDistance);
  QFontMetrics metrics = painter->fontMetrics();

  const QList<map::DistanceMarker>& distanceMarkers = mapPaintWidget->getDistanceMarkers();
  float lineWidth = context->szF(context->thicknessRangeDistance, 3);
  TextPlacement textPlacement(context->painter, this, QRect());

  for(const map::DistanceMarker& m : distanceMarkers)
  {
    // Get color from marker
    painter->setPen(QPen(m.color, lineWidth * 0.5, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));

    const int SYMBOL_SIZE = 5;
    int x, y;
    if(wToS(m.from, x, y))
    {
      // Draw ellipse at start point
      painter->setBrush(Qt::white);
      painter->drawEllipse(QPoint(x, y), SYMBOL_SIZE, SYMBOL_SIZE);
    }

    if(wToS(m.to, x, y))
    {
      // Draw cross at end point
      painter->drawLine(x - SYMBOL_SIZE, y, x + SYMBOL_SIZE, y);
      painter->drawLine(x, y - SYMBOL_SIZE, x, y + SYMBOL_SIZE);
    }

    painter->setPen(QPen(m.color, lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin));
    // Draw great circle line ========================================================
    float distanceMeter = m.from.distanceMeterTo(m.to);

    // Draw line
    drawLine(painter, Line(m.from, m.to));

    // Build and draw text
    QStringList texts;

    if(context->dOptMeasurement(optsd::MEASUREMNENT_LABEL) && !m.text.isEmpty())
      texts.append(m.text);

    GeoDataCoordinates from(m.from.getLonX(), m.from.getLatY(), 0, DEG);
    GeoDataCoordinates to(m.to.getLonX(), m.to.getLatY(), 0, DEG);
    double initTrue = normalizeCourse(from.bearing(to, DEG, INITBRG));
    double finalTrue = normalizeCourse(from.bearing(to, DEG, FINALBRG));
#ifdef DEBUG_INFORMATION_MEASUREMENT
    int precision = 3;
#else
    int precision = 0;
#endif
    QString initTrueText = QString::number(initTrue, 'f', precision);
    QString finalTrueText = QString::number(finalTrue, 'f', precision);
    QString initMagText = QString::number(normalizeCourse(initTrue - m.magvar), 'f', precision);
    QString finalMagText = QString::number(normalizeCourse(finalTrue -
                                                           NavApp::getMagVar(m.to, m.magvar)), 'f', precision);

#ifdef DEBUG_ALTERNATE_ARROW
    QString arrowLeft = ">> ";
#else
    QString arrowLeft = tr("► ");
#endif

    if(context->dOptMeasurement(optsd::MEASUREMNENT_TRUE) && context->dOptMeasurement(optsd::MEASUREMNENT_MAG) &&
       initTrueText == initMagText && finalTrueText == finalMagText)
    {
      if(initTrueText == finalTrueText)
        texts.append(initTrueText + tr("°M/T"));
      else
        texts.append(initTrueText + tr("°M/T ") + arrowLeft + finalTrueText + tr("°M/T"));
    }
    else
    {
      if(context->dOptMeasurement(optsd::MEASUREMNENT_MAG))
      {
        if(initMagText == finalMagText)
          texts.append(initMagText + tr("°M"));
        else
          texts.append(initMagText + tr("°M ") + arrowLeft + finalMagText + tr("°M"));
      }

      if(context->dOptMeasurement(optsd::MEASUREMNENT_TRUE))
      {
        if(initTrueText == finalTrueText)
          texts.append(initTrueText + tr("°T"));
        else
          texts.append(initTrueText + tr("°T ") + arrowLeft + finalTrueText + tr("°T"));
      }
    }

    if(context->dOptMeasurement(optsd::MEASUREMNENT_DIST))
    {
      if(Unit::getUnitDist() == opts::DIST_KM && Unit::getUnitShortDist() == opts::DIST_SHORT_METER &&
         distanceMeter < 6000)
        texts.append(QLocale(QLocale::C).toString(distanceMeter, 'f', 0) + Unit::getUnitShortDistStr());
      else
      {
        texts.append(Unit::distMeter(distanceMeter, true /* addUnit */, 20, true /* narrow */));
        if(distanceMeter < 6000)
          // Add feet to text for short distances
          texts.append(Unit::distShortMeter(distanceMeter, true /* addUnit */, true /* narrow */));
      }
    }

#ifdef DEBUG_INFORMATION_DISTANCE
    texts.append("[" + QString::number(distanceMeter, 'f', 0) + " m]");
#endif

    if(m.from != m.to && !texts.isEmpty())
    {
      int xt = -1, yt = -1;
      if(textPlacement.findTextPos(m.from, m.to, distanceMeter, metrics.width(texts.at(0)),
                                   metrics.height() * 2, xt, yt, nullptr))
        symbolPainter->textBox(painter, texts, painter->pen(), xt, yt, textatt::CENTER);
    }
  }
}

void MapPainterMark::paintTrafficPatterns()
{
  atools::util::PainterContextSaver saver(context->painter);
  GeoPainter *painter = context->painter;
  const QList<TrafficPattern>& patterns = mapPaintWidget->getTrafficPatterns();
  float lineWidth = context->szF(context->thicknessRangeDistance, 3);
  context->szFont(context->textSizeRangeDistance);

  TextPlacement textPlacement(painter, this, context->screenRect.marginsAdded(QMargins(50, 50, 50, 50)));
  textPlacement.setLineWidth(lineWidth);
  painter->setBackgroundMode(Qt::OpaqueMode);
  painter->setBackground(Qt::white);

  QPolygonF arrow = buildArrow(lineWidth * 2.3f);

  for(const TrafficPattern& pattern : patterns)
  {
    bool visibleOrigin, hiddenOrigin;
    QPointF originPoint = wToS(pattern.getPosition(), DEFAULT_WTOS_SIZE, &visibleOrigin, &hiddenOrigin);
    if(hiddenOrigin)
      continue;

    float finalDistance = pattern.base45Degree ? pattern.downwindDistance : pattern.baseDistance;
    if(context->mapLayer->isApproach() && scale->getPixelForNm(finalDistance) > 5.f)
    {

      // Turn point base to final
      Pos baseFinal = pattern.position.endpoint(nmToMeter(finalDistance),
                                                opposedCourseDeg(pattern.courseTrue));

      // Turn point downwind to base
      Pos downwindBase = baseFinal.endpoint(nmToMeter(pattern.downwindDistance),
                                            pattern.courseTrue + (pattern.turnRight ? 90.f : -90.f));

      // Turn point upwind to crosswind
      Pos upwindCrosswind = pattern.position.endpoint(nmToMeter(finalDistance) + feetToMeter(pattern.runwayLength),
                                                      pattern.courseTrue);

      // Turn point crosswind to downwind
      Pos crosswindDownwind = upwindCrosswind.endpoint(nmToMeter(pattern.downwindDistance),
                                                       pattern.courseTrue +
                                                       (pattern.turnRight ? 90.f : -90.f));

      // Calculate bounding rectangle and check if it is at least partially visible
      Rect rect(baseFinal);
      rect.extend(downwindBase);
      rect.extend(upwindCrosswind);
      rect.extend(crosswindDownwind);

      // Expand rect by approximately 2 NM
      rect.inflateMeter(atools::geo::nmToMeter(2.f), atools::geo::nmToMeter(2.f));

      if(context->viewportRect.overlaps(rect))
      {
        // Entry at opposite runway threshold
        Pos downwindEntry = downwindBase.endpoint(nmToMeter(finalDistance) + feetToMeter(
                                                    pattern.runwayLength), pattern.courseTrue);

        bool visible, hidden;
        // Bail out if any points are hidden behind the globe
        QPointF baseFinalPoint = wToS(baseFinal, DEFAULT_WTOS_SIZE, &visible, &hidden);
        if(hidden)
          continue;
        QPointF downwindBasePoint = wToS(downwindBase, DEFAULT_WTOS_SIZE, &visible, &hidden);
        if(hidden)
          continue;
        QPointF upwindCrosswindPoint = wToS(upwindCrosswind, DEFAULT_WTOS_SIZE, &visible, &hidden);
        if(hidden)
          continue;
        QPointF crosswindDownwindPoint = wToS(crosswindDownwind, DEFAULT_WTOS_SIZE, &visible, &hidden);
        if(hidden)
          continue;
        QPointF downwindEntryPoint = wToS(downwindEntry, DEFAULT_WTOS_SIZE, &visible, &hidden);
        if(hidden)
          continue;
        bool drawDetails = QLineF(baseFinalPoint, crosswindDownwindPoint).length() > 50.;

        // Calculate polygon rounding in pixels =======================
        float pixelForNm = scale->getPixelForNm(pattern.downwindDistance, pattern.courseTrue + 90.f);
        atools::util::RoundedPolygon polygon(pixelForNm / 3.f,
                                             {originPoint, upwindCrosswindPoint, crosswindDownwindPoint,
                                              downwindBasePoint, baseFinalPoint});

        QLineF downwind(crosswindDownwindPoint, downwindBasePoint);
        QLineF upwind(originPoint, upwindCrosswindPoint);
        float angle = static_cast<float>(angleFromQt(downwind.angle()));
        float oppAngle = static_cast<float>(opposedCourseDeg(angleFromQt(downwind.angle())));

        if(pattern.showEntryExit && context->mapLayer->isApproachText())
        {
          // Draw a line below to fill the gap because of round edges
          painter->setBrush(Qt::white);
          painter->setPen(QPen(pattern.color, context->szF(context->thicknessRangeDistance, 3), Qt::DashLine));
          drawLine(painter, upwind);

          // Straight out exit for pattern =======================
          QPointF exitStraight =
            wToS(upwindCrosswind.endpoint(nmToMeter(1.f), oppAngle),
                 DEFAULT_WTOS_SIZE, &visible, &hidden);
          drawLine(painter, upwind.p2(), exitStraight);

          // 45 degree exit for pattern =======================
          QPointF exit45Deg =
            wToS(upwindCrosswind.endpoint(nmToMeter(1.f), oppAngle + (pattern.turnRight ? 45.f : -45.f)),
                 DEFAULT_WTOS_SIZE, &visible, &hidden);
          drawLine(painter, upwind.p2(), exit45Deg);

          // Entry to downwind
          QPointF entry =
            wToS(downwindEntry.endpoint(nmToMeter(1.f), oppAngle + (pattern.turnRight ? 45.f : -45.f)),
                 DEFAULT_WTOS_SIZE, &visible, &hidden);
          drawLine(painter, downwindEntryPoint, entry);

          if(drawDetails)
          {
            // Draw arrows to all the entry and exit indicators ========================
            painter->setPen(QPen(pattern.color, context->szF(context->thicknessRangeDistance, 2), Qt::SolidLine));
            paintArrowAlongLine(painter, QLineF(upwind.p2(), exitStraight), arrow, 0.95f);
            paintArrowAlongLine(painter, QLineF(upwind.p2(), exit45Deg), arrow, 0.95f);
            paintArrowAlongLine(painter, QLineF(entry, downwindEntryPoint), arrow, 0.05f);
          }
        }

        painter->setPen(QPen(pattern.color, lineWidth));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(polygon.getPainterPath());

        if(drawDetails && context->mapLayer->isApproachText())
        {
          // Text for downwind leg =======================================
          QLineF final (baseFinalPoint, originPoint);
          QPointF center = downwind.center();
          QString text = tr("%1/%2").
                         arg(Unit::altFeet(pattern.position.getAltitude(), true /* addUnit */, true /* narrow */,
                                           10.f /* round */)).
                         arg(formatter::courseTextFromTrue(opposedCourseDeg(
                                                             pattern.courseTrue), pattern.magvar,
                                                           false /* magBold */, false /* trueSmall */,
                                                           true /* narrow */));

          painter->setBrush(Qt::white);
          textPlacement.drawTextAlongOneLine(text, angle, center, atools::roundToInt(downwind.length()),
                                             true /* both visible */);

          // Text for final leg =======================================
          text = tr("RW%1/%2").
                 arg(pattern.runwayName).
                 arg(formatter::courseTextFromTrue(pattern.courseTrue, pattern.magvar,
                                                   false /* magBold */, false /* trueSmall */, true /* narrow */));
          textPlacement.drawTextAlongOneLine(text, oppAngle, final.pointAt(0.60), atools::roundToInt(final.length()),
                                             true /* both visible */);

          // Draw arrows on legs =======================================
          // Set a lighter fill color for arrows

          painter->setBrush(pattern.color.lighter(300));
          painter->setPen(QPen(pattern.color, painter->pen().widthF() * 0.66));

          // Two arrows on downwind leg
          paintArrowAlongLine(painter, downwind, arrow, 0.75f);
          paintArrowAlongLine(painter, downwind, arrow, 0.25f);

          // Base leg
          paintArrowAlongLine(painter, QLineF(downwindBasePoint, baseFinalPoint), arrow);

          // Final leg
          paintArrowAlongLine(painter, final, arrow, 0.30f);

          // Upwind leg
          paintArrowAlongLine(painter, upwind, arrow);

          // Crosswind leg
          paintArrowAlongLine(painter, QLineF(upwindCrosswindPoint, crosswindDownwindPoint), arrow);
        }

      }
    }

    if(visibleOrigin)
    {
      // Draw ellipse at touchdown point - independent of zoom factor
      painter->setPen(QPen(pattern.color, lineWidth));
      painter->setBrush(Qt::white);
      painter->drawEllipse(originPoint, lineWidth * 2.f, lineWidth * 2.f);
    }
  }
}

void MapPainterMark::paintUserpointDrag()
{
  // Get screen position an pixmap
  QPoint cur;
  QPixmap pixmap;
  MapWidget *mapWidget = dynamic_cast<MapWidget *>(mapPaintWidget);
  if(mapWidget != nullptr)
    mapWidget->getUserpointDragPoints(cur, pixmap);

  if(!cur.isNull() && mapPaintWidget->rect().contains(cur) && !pixmap.isNull())
    context->painter->drawPixmap(cur.x() - pixmap.width() / 2, cur.y() - pixmap.height() / 2, pixmap);
}

/* Draw route dragging/moving lines */
void MapPainterMark::paintRouteDrag()
{
  LineString fixed;
  QPoint cur;

  MapWidget *mapWidget = dynamic_cast<MapWidget *>(mapPaintWidget);
  if(mapWidget != nullptr)
    mapWidget->getRouteDragPoints(fixed, cur);

  if(!cur.isNull())
  {
    Pos curGeo;
    if(sToW(cur.x(), cur.y(), curGeo))
    {
      context->painter->setPen(QPen(mapcolors::mapDragColor, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

      // Draw lines from current mouse position to all fixed points which can be waypoints or several alternates
      for(const Pos& pos : fixed)
        drawLine(context->painter, Line(curGeo, pos));
    }
  }
}
