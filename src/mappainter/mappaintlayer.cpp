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

#include "mappainter/mappaintlayer.h"

#include "navapp.h"
#include "mappainter/mappainterweather.h"
#include "mappainter/mappainterwind.h"
#include "connect/connectclient.h"
#include "common/mapcolors.h"
#include "mapgui/mapwidget.h"
#include "mapgui/maplayersettings.h"
#include "mappainter/mappainteraircraft.h"
#include "mappainter/mappaintertrack.h"
#include "mappainter/mappaintership.h"
#include "mappainter/mappainterairport.h"
#include "mappainter/mappainterairspace.h"
#include "mappainter/mappainterils.h"
#include "mappainter/mappaintermark.h"
#include "mappainter/mappainternav.h"
#include "mappainter/mappainterroute.h"
#include "mappainter/mappainteruser.h"
#include "mappainter/mappainteraltitude.h"
#include "mappainter/mappaintertop.h"
#include "mapgui/mapscale.h"
#include "userdata/userdatacontroller.h"
#include "route/route.h"
#include "geo/calculations.h"
#include "options/optiondata.h"

#include <QElapsedTimer>

#include <marble/GeoPainter.h>

using namespace Marble;
using namespace atools::geo;

MapPaintLayer::MapPaintLayer(MapPaintWidget *widget)
  : mapWidget(widget)
{
  // Create the layer configuration
  initMapLayerSettings();

  mapScale = new MapScale();

  // Create all painters
  mapPainterNav = new MapPainterNav(mapWidget, mapScale, &context);
  mapPainterIls = new MapPainterIls(mapWidget, mapScale, &context);
  mapPainterAirport = new MapPainterAirport(mapWidget, mapScale, &context);
  mapPainterAirspace = new MapPainterAirspace(mapWidget, mapScale, &context);
  mapPainterMark = new MapPainterMark(mapWidget, mapScale, &context);
  mapPainterRoute = new MapPainterRoute(mapWidget, mapScale, &context);
  mapPainterAircraft = new MapPainterAircraft(mapWidget, mapScale, &context);
  mapPainterTrack = new MapPainterTrack(mapWidget, mapScale, &context);
  mapPainterShip = new MapPainterShip(mapWidget, mapScale, &context);
  mapPainterUser = new MapPainterUser(mapWidget, mapScale, &context);
  mapPainterAltitude = new MapPainterAltitude(mapWidget, mapScale, &context);
  mapPainterWeather = new MapPainterWeather(mapWidget, mapScale, &context);
  mapPainterWind = new MapPainterWind(mapWidget, mapScale, &context);
  mapPainterTop = new MapPainterTop(mapWidget, mapScale, &context);

  // Default for visible object types
  objectTypes = map::MapTypes(map::AIRPORT | map::VOR | map::NDB | map::AP_ILS | map::MARKER | map::WAYPOINT);
  objectDisplayTypes = map::DISPLAY_TYPE_NONE;
}

MapPaintLayer::~MapPaintLayer()
{
  delete mapPainterNav;
  delete mapPainterIls;
  delete mapPainterAirport;
  delete mapPainterAirspace;
  delete mapPainterMark;
  delete mapPainterRoute;
  delete mapPainterAircraft;
  delete mapPainterTrack;
  delete mapPainterShip;
  delete mapPainterUser;
  delete mapPainterAltitude;
  delete mapPainterWeather;
  delete mapPainterWind;
  delete mapPainterTop;

  delete layers;
  delete mapScale;
}

void MapPaintLayer::copySettings(const MapPaintLayer& other)
{
  objectTypes = other.objectTypes;
  objectDisplayTypes = other.objectDisplayTypes;
  airspaceTypes = other.airspaceTypes;
  weatherSource = other.weatherSource;
  sunShading = other.sunShading;

  // Updates layers too
  setDetailFactor(other.detailFactor);
}

void MapPaintLayer::preDatabaseLoad()
{
  databaseLoadStatus = true;
}

void MapPaintLayer::postDatabaseLoad()
{
  databaseLoadStatus = false;
}

void MapPaintLayer::setShowMapObjects(map::MapTypes type, bool show)
{
  if(show)
    objectTypes |= type;
  else
    objectTypes &= ~type;
}

void MapPaintLayer::setShowMapObjectsDisplay(map::MapObjectDisplayTypes type, bool show)
{
  if(show)
    objectDisplayTypes |= type;
  else
    objectDisplayTypes &= ~type;
}

void MapPaintLayer::setShowAirspaces(map::MapAirspaceFilter types)
{
  airspaceTypes = types;
}

void MapPaintLayer::setDetailFactor(int factor)
{
  detailFactor = factor;
  updateLayers();
}

map::MapAirspaceFilter MapPaintLayer::getShownAirspacesTypesByLayer() const
{
  // Mask out all types that are not visible in the current layer
  map::MapAirspaceFilter filter = airspaceTypes;
  if(!mapLayer->isAirspaceIcao())
    filter.types &= ~map::AIRSPACE_CLASS_ICAO;

  if(!mapLayer->isAirspaceFg())
    filter.types &= ~map::AIRSPACE_CLASS_FG;

  if(!mapLayer->isAirspaceFirUir())
    filter.types &= ~map::AIRSPACE_FIR_UIR;

  if(!mapLayer->isAirspaceCenter())
    filter.types &= ~map::AIRSPACE_CENTER;

  if(!mapLayer->isAirspaceRestricted())
    filter.types &= ~map::AIRSPACE_RESTRICTED;

  if(!mapLayer->isAirspaceSpecial())
    filter.types &= ~map::AIRSPACE_SPECIAL;

  if(!mapLayer->isAirspaceOther())
    filter.types &= ~map::AIRSPACE_OTHER;

  return filter;
}

/* Initialize the layer settings that define what is drawn at what zoom distance (text, size, etc.) */
void MapPaintLayer::initMapLayerSettings()
{
  // TODO move to configuration file
  if(layers != nullptr)
    delete layers;

  // =====================================================================================
  // Create a list of map layers that define content for each zoom distance
  layers = new MapLayerSettings();

  // Create a default layer with all features enabled
  // Features are switched off step by step when adding new (higher) layers
  MapLayer defLayer = MapLayer(0).airport().approach().approachText().approachDetail().airportName().airportIdent().
                      airportSoft().airportNoRating().airportOverviewRunway().airportSource(layer::ALL).

                      airportWeather().airportWeatherDetails().

                      windBarbs().

                      routeTextAndDetail().

                      minimumAltitude().

                      vor().ndb().waypoint().marker().ils().airway().track().

                      userpoint().userpointInfo().

                      aiAircraftGround().aiAircraftLarge().aiAircraftSmall().aiShipLarge().aiShipSmall().
                      aiAircraftGroundText().aiAircraftText().

                      onlineAircraft().onlineAircraftText().

                      airspaceCenter().airspaceFg().airspaceFirUir().airspaceOther().airspaceRestricted().
                      airspaceSpecial().airspaceIcao().

                      vorRouteIdent().vorRouteInfo().ndbRouteIdent().ndbRouteInfo().waypointRouteName().
                      airportRouteInfo();

  // Lowest layer including everything (airport diagram and details)
  // airport diagram, large VOR, NDB, ILS, waypoint, airway, marker
  layers->
  append(defLayer.clone(0.2f).airportDiagramRunway().airportDiagram().
         airportDiagramDetail().airportDiagramDetail2().airportDiagramDetail3().
         airportSymbolSize(20).airportInfo().
         windBarbsSymbolSize(22).
         waypointSymbolSize(14).waypointName().
         vorSymbolSize(30).vorIdent().vorInfo().vorLarge().
         ndbSymbolSize(30).ndbIdent().ndbInfo().
         ilsIdent().ilsInfo().
         holding().holdingInfo().holdingInfo2().
         airwayIdent().airwayInfo().airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(28).userpointMaxTextLength(30).
         markerSymbolSize(24).markerInfo().
         airportMaxTextLength(30)).

  append(defLayer.clone(0.3f).airportDiagramRunway().airportDiagram().airportDiagramDetail().airportDiagramDetail2().
         airportSymbolSize(20).airportInfo().
         windBarbsSymbolSize(20).
         waypointSymbolSize(14).waypointName().
         vorSymbolSize(30).vorIdent().vorInfo().vorLarge().
         ndbSymbolSize(30).ndbIdent().ndbInfo().
         ilsIdent().ilsInfo().
         holding().holdingInfo().holdingInfo2().
         airwayIdent().airwayInfo().airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(28).userpointMaxTextLength(30).
         markerSymbolSize(24).markerInfo().
         airportMaxTextLength(30)).

  // airport diagram, large VOR, NDB, ILS, waypoint, airway, marker
  append(defLayer.clone(1.f).airportDiagramRunway().airportDiagram().airportDiagramDetail().
         airportSymbolSize(20).airportInfo().
         windBarbsSymbolSize(20).
         aiAircraftGroundText(false).
         waypointSymbolSize(14).waypointName().
         vorSymbolSize(28).vorIdent().vorInfo().vorLarge().
         ndbSymbolSize(28).ndbIdent().ndbInfo().
         ilsIdent().ilsInfo().
         holding().holdingInfo().holdingInfo2().
         airwayIdent().airwayInfo().airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(28).userpointMaxTextLength(30).
         markerSymbolSize(24).markerInfo().
         airportMaxTextLength(30)).

  // airport diagram, large VOR, NDB, ILS, waypoint, airway, marker
  append(defLayer.clone(5.f).airportDiagramRunway().airportDiagram().
         airportSymbolSize(20).airportInfo().
         waypointSymbolSize(10).waypointName().
         windBarbsSymbolSize(18).
         aiAircraftGroundText(false).
         vorSymbolSize(26).vorIdent().vorInfo().vorLarge().
         ndbSymbolSize(26).ndbIdent().ndbInfo().
         ilsIdent().ilsInfo().
         holding().holdingInfo().holdingInfo2().
         airwayIdent().airwayInfo().airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(26).userpointMaxTextLength(20).
         markerSymbolSize(24).markerInfo().
         airportMaxTextLength(30)).

  // airport, large VOR, NDB, ILS, waypoint, airway, marker
  append(defLayer.clone(10.f).airportDiagramRunway().
         airportSymbolSize(18).airportInfo().
         waypointSymbolSize(8).waypointName().
         windBarbsSymbolSize(16).
         aiAircraftGroundText(false).
         vorSymbolSize(24).vorIdent().vorInfo().vorLarge().
         ndbSymbolSize(24).ndbIdent().ndbInfo().
         ilsIdent().ilsInfo().
         holding().holdingInfo().holdingInfo2().
         airwayIdent().airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(26).userpointMaxTextLength(20).
         markerSymbolSize(24).
         airportMaxTextLength(20)).

  // airport, large VOR, NDB, ILS, waypoint, airway, marker
  append(defLayer.clone(25.f).airportDiagramRunway().
         airportSymbolSize(18).airportInfo().
         waypointSymbolSize(8).waypointName().
         windBarbsSymbolSize(16).
         aiAircraftGroundText(false).
         vorSymbolSize(22).vorIdent().vorInfo().vorLarge().
         ndbSymbolSize(22).ndbIdent().ndbInfo().
         ilsIdent().ilsInfo().
         holding().holdingInfo().holdingInfo2().
         airwayIdent().airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(24).userpointMaxTextLength(20).
         markerSymbolSize(24).
         airportMaxTextLength(20)).

  // airport, large VOR, NDB, ILS, airway
  append(defLayer.clone(50.f).
         airportSymbolSize(16).airportInfo().
         waypointSymbolSize(6).
         windBarbsSymbolSize(16).
         aiShipSmall(false).aiAircraftGroundText(false).aiAircraftText(false).
         vorSymbolSize(20).vorIdent().vorInfo().vorLarge().
         ndbSymbolSize(20).ndbIdent().ndbInfo().
         holding().holdingInfo().
         airwayIdent().airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(24).userpointMaxTextLength(10).
         marker(false).
         airportMaxTextLength(16)).

  // airport, VOR, NDB, ILS, airway
  append(defLayer.clone(100.f).
         airportSymbolSize(16).
         waypointSymbolSize(3).
         windBarbsSymbolSize(14).
         aiAircraftGround(false).aiShipSmall(false).aiAircraftGroundText(false).aiAircraftText(false).
         vorSymbolSize(20).vorIdent().
         ndbSymbolSize(16).ndbIdent().
         holding().
         airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(24).userpointMaxTextLength(10).
         marker(false).
         airportMaxTextLength(16)).

  // airport, VOR, NDB, airway
  append(defLayer.clone(150.f).
         airportSymbolSize(12).minRunwayLength(2500).
         airportOverviewRunway(false).airportName(false).
         windBarbsSymbolSize(14).
         approachText(false).
         aiAircraftGround(false).aiShipSmall(false).aiAircraftGroundText(false).aiAircraftText(false).
         waypoint(false).
         vorSymbolSize(12).ndbSymbolSize(12).
         holding().
         airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         userpoint().userpointInfo().userpointSymbolSize(22).userpointMaxTextLength(8).
         marker(false).
         airportMaxTextLength(16)).

  // airport > 4000, VOR
  append(defLayer.clone(200.f).airportSymbolSize(12).minRunwayLength(layer::MAX_MEDIUM_RUNWAY_FT).
         airportOverviewRunway(false).airportName(false).airportSource(layer::MEDIUM).
         windBarbsSymbolSize(14).
         approachText(false).
         aiAircraftGround(false).aiShipSmall(false).aiAircraftGroundText(false).aiAircraftText(false).
         onlineAircraftText(false).
         airwayWaypoint().
         trackIdent().trackInfo().trackWaypoint().
         vorSymbolSize(8).ndbSymbolSize(8).waypoint(false).marker(false).
         holding().
         userpoint().userpointInfo().userpointSymbolSize(16).userpointMaxTextLength(8).
         airportMaxTextLength(16)).

  // airport > 4000
  append(defLayer.clone(300.f).airportSymbolSize(10).minRunwayLength(layer::MAX_MEDIUM_RUNWAY_FT).
         airportOverviewRunway(false).airportName(false).airportSource(layer::MEDIUM).
         windBarbsSymbolSize(12).
         approachText(false).
         aiAircraftGround(false).aiShipSmall(false).
         aiAircraftGroundText(false).aiAircraftText(false).
         aiAircraftSize(26).
         onlineAircraftText(false).
         trackIdent().trackInfo(false).trackWaypoint().
         vorSymbolSize(6).ndbSymbolSize(4).waypoint(false).marker(false).ils(false).
         holding().
         trackIdent().trackInfo(false).trackWaypoint().
         airportRouteInfo(false).waypointRouteName(false).
         userpoint().userpointInfo(false).userpointSymbolSize(16).
         airportMaxTextLength(16)).

  // airport > 8000
  append(defLayer.clone(750.f).airportSymbolSize(8).minRunwayLength(layer::MAX_LARGE_RUNWAY_FT).
         airportOverviewRunway(false).airportName(false).airportSource(layer::LARGE).
         windBarbsSymbolSize(12).
         approachText(false).
         aiAircraftGround(false).aiShipLarge(false).aiShipSmall(false).
         aiAircraftGroundText(false).aiAircraftText(false).
         aiAircraftSize(24).
         onlineAircraftText(false).
         airspaceOther(false).airspaceRestricted(false).airspaceSpecial(false).
         vorSymbolSize(3).ndb(false).waypoint(false).marker(false).ils(false).airway(false).
         trackIdent().trackInfo(false).trackWaypoint().
         airportRouteInfo(false).vorRouteInfo(false).ndbRouteInfo(false).waypointRouteName(false).
         userpoint().userpointInfo(false).userpointSymbolSize(12).
         airportMaxTextLength(16)).

  // airport > 8000
  append(defLayer.clone(1200.f).airportSymbolSize(6).minRunwayLength(layer::MAX_LARGE_RUNWAY_FT).
         airportOverviewRunway(false).airportName(false).airportSource(layer::LARGE).
         windBarbsSymbolSize(10).
         approachText(false).approachDetail(false).
         aiAircraftGround(false).aiAircraftSmall(false).aiShipLarge(false).aiShipSmall(false).
         aiAircraftGroundText(false).aiAircraftText(false).
         aiAircraftSize(20).
         onlineAircraftText(false).
         airspaceFg(false).airspaceOther(false).airspaceRestricted(false).airspaceSpecial(false).
         airspaceIcao(false).
         vor(false).ndb(false).waypoint(false).marker(false).ils(false).airway(false).
         trackIdent().trackInfo(false).trackWaypoint(false).
         airportRouteInfo(false).vorRouteInfo(false).ndbRouteInfo(false).waypointRouteName(false).
         userpoint().userpointInfo(false).userpointSymbolSize(12).
         airportMaxTextLength(16)).

  // Display only points for airports until the cutoff limit
  // airport > 8000
  append(defLayer.clone(2400.f).airportSymbolSize(4).
         minRunwayLength(layer::MAX_LARGE_RUNWAY_FT).
         airportOverviewRunway(false).airportName(false).airportIdent(false).airportSource(layer::LARGE).
         airportWeather(false).airportWeatherDetails(false).
         windBarbsSymbolSize(6).
         minimumAltitude(false).
         approachText(false).approachDetail(false).
         aiAircraftGround(false).aiAircraftSmall(false).aiShipLarge(false).aiShipSmall(false).
         aiAircraftGroundText(false).aiAircraftText(false).
         aiAircraftSize(10).
         onlineAircraftText(false).
         airspaceCenter(false).airspaceFg(false).airspaceOther(false).
         airspaceRestricted(false).airspaceSpecial(false).airspaceIcao(false).
         vor(false).ndb(false).waypoint(false).marker(false).ils(false).airway(false).
         trackIdent().trackInfo(false).trackWaypoint(false).
         airportRouteInfo(false).vorRouteInfo(false).ndbRouteInfo(false).waypointRouteName(false).
         userpoint().userpointInfo(false).userpointSymbolSize(12).
         airportMaxTextLength(16)).

  append(defLayer.clone(layer::DISTANCE_CUT_OFF_LIMIT).
         airportSymbolSize(3).minRunwayLength(layer::MAX_LARGE_RUNWAY_FT).
         airportOverviewRunway(false).airportName(false).airportIdent(false).airportSource(layer::LARGE).
         airportWeather(false).airportWeatherDetails(false).
         windBarbs(false).
         minimumAltitude(false).
         approach(false).approachText(false).approachDetail(false).
         aiAircraftGround(false).aiAircraftLarge(false).aiAircraftSmall(false).aiShipLarge(false).aiShipSmall(false).
         aiAircraftGroundText(false).aiAircraftText(false).
         aiAircraftSize(10).
         onlineAircraftText(false).
         airspaceCenter(false).airspaceFirUir(false).airspaceFg(false).airspaceOther(false).
         airspaceRestricted(false).airspaceSpecial(false).airspaceIcao(false).
         vor(false).ndb(false).waypoint(false).marker(false).ils(false).airway(false).
         trackIdent().trackInfo(false).trackWaypoint(false).
         airportRouteInfo(false).vorRouteInfo(false).ndbRouteInfo(false).waypointRouteName(false).
         userpoint().userpointInfo(false).userpointSymbolSize(12).
         airportMaxTextLength(16)).

  // Make sure that there is always a layer
  append(defLayer.clone(100000.f).
         airportSymbolSize(3).minRunwayLength(layer::MAX_LARGE_RUNWAY_FT).
         airportOverviewRunway(false).airportName(false).airportIdent(false).airportSource(layer::LARGE).
         airportWeather(false).airportWeatherDetails(false).
         windBarbs(false).
         minimumAltitude(false).
         routeTextAndDetail(false).
         approach(false).approachText(false).approachDetail(false).
         aiAircraftGround(false).aiAircraftLarge(false).aiAircraftSmall(false).aiShipLarge(false).aiShipSmall(false).
         aiAircraftGroundText(false).aiAircraftText(false).
         onlineAircraft(false).onlineAircraftText(false).
         airspaceCenter(false).airspaceFirUir(false).airspaceFg(false).airspaceOther(false).
         airspaceRestricted(false).airspaceSpecial(false).airspaceIcao(false).
         airport(false).vor(false).ndb(false).waypoint(false).marker(false).ils(false).airway(false).track(false).
         airportRouteInfo(false).vorRouteInfo(false).ndbRouteInfo(false).waypointRouteName(false).
         userpoint(false).userpointInfo(false).userpointSymbolSize(12).
         airportMaxTextLength(16));

  // Sort layers
  layers->finishAppend();
  qDebug() << *layers;
}

/* Update the stored layer pointers after zoom distance has changed */
void MapPaintLayer::updateLayers()
{
  float dist = static_cast<float>(mapWidget->distance());
  // Get the uncorrected effective layer - route painting is independent of declutter
  mapLayerEffective = layers->getLayer(dist);
  mapLayer = layers->getLayer(dist, detailFactor);
  mapLayerRoute = layers->getLayer(dist, detailFactor + 1);
}

bool MapPaintLayer::render(GeoPainter *painter, ViewportParams *viewport, const QString& renderPos,
                           GeoSceneLayer *layer)
{
  Q_UNUSED(renderPos)
  Q_UNUSED(layer)

  if(!databaseLoadStatus && !mapWidget->isNoNavPaint())
  {
    // Update map scale for screen distance approximation
    mapScale->update(viewport, mapWidget->distance());
    updateLayers();

    // What to draw while scrolling or zooming map
    opts::MapScrollDetail mapScrollDetail = OptionData::instance().getMapScrollDetail();

    // Check if no painting wanted during scroll
    if(!(mapScrollDetail == opts::NONE && mapWidget->viewContext() == Marble::Animation) && // Performance settings
       !(viewport->projection() == Marble::Mercator && // Do not draw if Mercator wraps around whole planet
         viewport->viewLatLonAltBox().width(GeoDataCoordinates::Degree) >= 359.))
    {
#ifdef DEBUG_INFORMATION_PAINT
      qDebug() << Q_FUNC_INFO << "layer" << *mapLayer;
#endif

      context = PaintContext();
      context.route = &NavApp::getRouteConst();
      context.mapLayer = mapLayer;
      context.mapLayerRoute = mapLayerRoute;
      context.mapLayerEffective = mapLayerEffective;
      context.painter = painter;
      context.viewport = viewport;
      context.objectTypes = objectTypes;
      context.objectDisplayTypes = objectDisplayTypes;
      context.airspaceFilterByLayer = getShownAirspacesTypesByLayer();
      context.viewContext = mapWidget->viewContext();
      context.drawFast = (mapScrollDetail == opts::FULL || mapScrollDetail == opts::HIGHER) ?
                         false : mapWidget->viewContext() == Marble::Animation;
      context.lazyUpdate = mapScrollDetail == opts::FULL ? false : mapWidget->viewContext() == Marble::Animation;
      context.mapScrollDetail = mapScrollDetail;
      context.distance = atools::geo::meterToNm(static_cast<float>(mapWidget->distance() * 1000.));

      context.userPointTypes = NavApp::getUserdataController()->getSelectedTypes();
      context.userPointTypesAll = NavApp::getUserdataController()->getAllTypes();
      context.userPointTypeUnknown = NavApp::getUserdataController()->isSelectedUnknownType();
      context.zoomDistanceMeter = static_cast<float>(mapWidget->distance() * 1000.);
      context.darkMap = mapWidget->isDarkMap();

      // Copy default font
      context.defaultFont = painter->font();
      painter->setFont(context.defaultFont);

      const GeoDataLatLonAltBox& box = viewport->viewLatLonAltBox();
      context.viewportRect = atools::geo::Rect(box.west(GeoDataCoordinates::Degree),
                                               box.north(GeoDataCoordinates::Degree),
                                               box.east(GeoDataCoordinates::Degree),
                                               box.south(GeoDataCoordinates::Degree));

      context.screenRect = mapWidget->rect();

      const OptionData& od = OptionData::instance();

      context.symbolSizeAircraftAi = od.getDisplaySymbolSizeAircraftAi() / 100.f;
      context.symbolSizeAircraftUser = od.getDisplaySymbolSizeAircraftUser() / 100.f;
      context.symbolSizeAirport = od.getDisplaySymbolSizeAirport() / 100.f;
      context.symbolSizeAirportWeather = od.getDisplaySymbolSizeAirportWeather() / 100.f;
      context.symbolSizeWindBarbs = od.getDisplaySymbolSizeWindBarbs() / 100.f;
      context.symbolSizeNavaid = od.getDisplaySymbolSizeNavaid() / 100.f;
      context.textSizeAircraftAi = od.getDisplayTextSizeAircraftAi() / 100.f;
      context.textSizeAircraftUser = od.getDisplayTextSizeAircraftUser() / 100.f;
      context.textSizeAirport = od.getDisplayTextSizeAirport() / 100.f;
      context.textSizeFlightplan = od.getDisplayTextSizeFlightplan() / 100.f;
      context.textSizeNavaid = od.getDisplayTextSizeNavaid() / 100.f;
      context.textSizeAirway = od.getDisplayTextSizeAirway() / 100.f;
      context.textSizeCompassRose = od.getDisplayTextSizeCompassRose() / 100.f;
      context.textSizeMora = od.getDisplayTextSizeMora() / 100.f;
      context.transparencyMora = od.getDisplayTransparencyMora() / 100.f;
      context.textSizeRangeDistance = od.getDisplayTextSizeRangeDistance() / 100.f;
      context.thicknessFlightplan = od.getDisplayThicknessFlightplan() / 100.f;
      context.thicknessTrail = od.getDisplayThicknessTrail() / 100.f;
      context.thicknessRangeDistance = od.getDisplayThicknessRangeDistance() / 100.f;
      context.thicknessCompassRose = od.getDisplayThicknessCompassRose() / 100.f;
      context.thicknessAirway = od.getDisplayThicknessAirway() / 100.f;

      context.dispOptsUser = od.getDisplayOptionsUserAircraft();
      context.dispOptsAi = od.getDisplayOptionsAiAircraft();
      context.dispOptsAirport = od.getDisplayOptionsAirport();
      context.dispOptsRose = od.getDisplayOptionsRose();
      context.dispOptsMeasurement = od.getDisplayOptionsMeasurement();
      context.dispOptsRoute = od.getDisplayOptionsRoute();
      context.flags = od.getFlags();
      context.flags2 = od.getFlags2();

      context.weatherSource = weatherSource;
      context.visibleWidget = mapWidget->isVisibleWidget();

      // ====================================
      // Get all waypoints from the route and add them to the map to avoid duplicate drawing
      if(context.objectDisplayTypes.testFlag(map::FLIGHTPLAN))
      {
        const Route& route = NavApp::getRouteConst();
        for(int i = 0; i < route.size(); i++)
        {
          const RouteLeg& routeLeg = route.value(i);
          map::MapTypes type = routeLeg.getMapObjectType();
          if(type == map::AIRPORT || type == map::VOR || type == map::NDB || type == map::WAYPOINT)
            context.routeProcIdMap.insert(map::MapObjectRef(routeLeg.getId(), routeLeg.getMapObjectType()));
          else if(type == map::PROCEDURE)
          {
            if(!routeLeg.getProcedureLeg().isMissed() || context.objectTypes & map::MISSED_APPROACH)
            {
              const map::MapResult& navaids = routeLeg.getProcedureLeg().navaids;
              if(navaids.hasWaypoints())
                context.routeProcIdMap.insert({navaids.waypoints.first().id, map::WAYPOINT});
              if(navaids.hasVor())
                context.routeProcIdMap.insert({navaids.vors.first().id, map::VOR});
              if(navaids.hasNdb())
                context.routeProcIdMap.insert({navaids.ndbs.first().id, map::NDB});
            }
          }
        }
      }

      // ====================================
      // Get navaids from procedure highlight to avoid duplicate drawing

      if(context.mapLayerRoute->isApproach())
      {
        const proc::MapProcedureLegs& procs = mapWidget->getProcedureHighlight();
        for(int i = 0; i < procs.size(); i++)
        {
          const map::MapResult& navaids = procs.at(i).navaids;
          if(navaids.hasWaypoints())
            context.routeProcIdMap.insert({navaids.waypoints.first().id, map::WAYPOINT});
          if(navaids.hasVor())
            context.routeProcIdMap.insert({navaids.vors.first().id, map::VOR});
          if(navaids.hasNdb())
            context.routeProcIdMap.insert({navaids.ndbs.first().id, map::NDB});
        }
      }

      // ====================================
      // Get airports from logbook highlight to avoid duplicate drawing
      const map::MapResult& highlightResultsSearch = mapWidget->getSearchHighlights();
      for(const map::MapLogbookEntry& entry : highlightResultsSearch.logbookEntries)
      {
        if(entry.departurePos.isValid())
          context.routeProcIdMap.insert({entry.departure.id, map::AIRPORT});
        if(entry.destinationPos.isValid())
          context.routeProcIdMap.insert({entry.destination.id, map::AIRPORT});
      }

      // Set render hints depending on context (moving, still) =====================
      if(mapWidget->viewContext() == Marble::Still)
      {
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::TextAntialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
      }
      else if(mapWidget->viewContext() == Marble::Animation)
      {
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setRenderHint(QPainter::TextAntialiasing, false);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
      }

      // =========================================================================
      // Draw ====================================

      // Altitude below all others
      mapPainterAltitude->render();

      // Ship below other navaids and airports
      mapPainterShip->render();

      if(mapWidget->distance() < layer::DISTANCE_CUT_OFF_LIMIT)
      {
        if(!context.isObjectOverflow())
          mapPainterAirspace->render();

        if(context.mapLayer->isAirportDiagram())
        {
          // Put ILS below and navaids on top of airport diagram
          if(!context.isObjectOverflow())
            mapPainterIls->render();

          if(!context.isObjectOverflow())
            mapPainterAirport->render();

          if(!context.isObjectOverflow())
            mapPainterNav->render();
        }
        else
        {
          // Airports on top of all
          if(!context.isObjectOverflow())
            mapPainterIls->render();

          if(!context.isObjectOverflow())
            mapPainterNav->render();

          if(!context.isObjectOverflow())
            mapPainterAirport->render();
        }
      }

      if(!context.isObjectOverflow())
        mapPainterUser->render();

      if(!context.isObjectOverflow())
        mapPainterWind->render();

      // if(!context.isOverflow()) always paint route even if number of objects is too large
      mapPainterRoute->render();

      if(!context.isObjectOverflow())
        mapPainterWeather->render();

      if(!context.isObjectOverflow())
        mapPainterTrack->render();

      mapPainterMark->render();

      mapPainterAircraft->render();

      mapPainterTop->render();
    }

    if(!mapWidget->isPrinting() && mapWidget->isVisibleWidget())
      // Dim the map by drawing a semi-transparent black rectangle - but not for printing or web services
      mapcolors::darkenPainterRect(*painter);
  }
  return true;
}
