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

#ifndef LITTLENAVMAP_MAPPAINTERMARK_H
#define LITTLENAVMAP_MAPPAINTERMARK_H

#include "mappainter/mappainter.h"

namespace Marble {
class GeoDataLineString;
}

namespace map {
struct MapAirspace;
struct MapAirway;
struct MapLogbookEntry;
}

class MapWidget;

/*
 * Paint all marks, distance measure lines, range rings, selected object highlights
 * and magnetic pole indications.
 */
class MapPainterMark :
  public MapPainter
{
  Q_DECLARE_TR_FUNCTIONS(MapPainterMark)

public:
  MapPainterMark(MapPaintWidget *mapWidgetParam, MapScale *mapScale, PaintContext *paintContext);
  virtual ~MapPainterMark() override;

  virtual void render() override;

private:
  void paintMark();
  void paintHome();
  void paintHighlights();
  void paintRangeRings();
  void paintDistanceMarkers();

  void paintTrafficPatterns();

  void paintRouteDrag();
  void paintCompassRose();
  void paintUserpointDrag();
  void paintAirspace(const map::MapAirspace& airspace);
  void paintAirwayList(const QList<map::MapAirway>& airwayList);
  void paintAirwayTextList(const QList<map::MapAirway>& airwayList);
  void paintLogEntries(const QList<map::MapLogbookEntry>& entries);

};

#endif // LITTLENAVMAP_MAPPAINTERMARK_H
