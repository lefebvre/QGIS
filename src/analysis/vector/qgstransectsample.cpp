/***************************************************************************
    qgstransectsample.cpp
    ---------------------
    begin                : July 2013
    copyright            : (C) 2013 by Marco Hugentobler
    email                : marco dot hugentobler at sourcepole dot ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgstransectsample.h"
#include "qgsdistancearea.h"
#include "qgsfeatureiterator.h"
#include "qgsgeometry.h"
#include "qgsspatialindex.h"
#include "qgsvectorfilewriter.h"
#include "qgsvectorlayer.h"
#include "qgsproject.h"
#include "qgsfeedback.h"

#include <QFileInfo>
#ifndef _MSC_VER
#include <cstdint>
#endif
#include "mersenne-twister.h"
#include <limits>

QgsTransectSample::QgsTransectSample( QgsVectorLayer *strataLayer, const QString &strataIdAttribute, const QString &minDistanceAttribute, const QString &nPointsAttribute, DistanceUnits minDistUnits,
                                      QgsVectorLayer *baselineLayer, bool shareBaseline, const QString &baselineStrataId, const QString &outputPointLayer,
                                      const QString &outputLineLayer, const QString &usedBaselineLayer, double minTransectLength,
                                      double baselineBufferDistance, double baselineSimplificationTolerance )
  : mStrataLayer( strataLayer )
  , mStrataIdAttribute( strataIdAttribute )
  , mMinDistanceAttribute( minDistanceAttribute )
  , mNPointsAttribute( nPointsAttribute )
  , mBaselineLayer( baselineLayer )
  , mShareBaseline( shareBaseline )
  , mBaselineStrataId( baselineStrataId )
  , mOutputPointLayer( outputPointLayer )
  , mOutputLineLayer( outputLineLayer )
  , mUsedBaselineLayer( usedBaselineLayer )
  , mMinDistanceUnits( minDistUnits )
  , mMinTransectLength( minTransectLength )
  , mBaselineBufferDistance( baselineBufferDistance )
  , mBaselineSimplificationTolerance( baselineSimplificationTolerance )
{
}

QgsTransectSample::QgsTransectSample()
  : mStrataLayer( nullptr )
  , mBaselineLayer( nullptr )
  , mShareBaseline( false )
  , mMinDistanceUnits( Meters )
  , mMinTransectLength( 0.0 )
  , mBaselineBufferDistance( -1.0 )
  , mBaselineSimplificationTolerance( -1.0 )
{
}

int QgsTransectSample::createSample( QgsFeedback *feedback )
{
  if ( !mStrataLayer || !mStrataLayer->isValid() )
  {
    return 1;
  }

  if ( !mBaselineLayer || !mBaselineLayer->isValid() )
  {
    return 2;
  }

  //stratum id is not necessarily an integer
  QVariant::Type stratumIdType = QVariant::Int;
  if ( !mStrataIdAttribute.isEmpty() )
  {
    stratumIdType = mStrataLayer->fields().field( mStrataIdAttribute ).type();
  }

  //create vector file writers for output
  QgsFields outputPointFields;
  outputPointFields.append( QgsField( QStringLiteral( "id" ), stratumIdType ) );
  outputPointFields.append( QgsField( QStringLiteral( "station_id" ), QVariant::Int ) );
  outputPointFields.append( QgsField( QStringLiteral( "stratum_id" ), stratumIdType ) );
  outputPointFields.append( QgsField( QStringLiteral( "station_code" ), QVariant::String ) );
  outputPointFields.append( QgsField( QStringLiteral( "start_lat" ), QVariant::Double ) );
  outputPointFields.append( QgsField( QStringLiteral( "start_long" ), QVariant::Double ) );

  QgsVectorFileWriter outputPointWriter( mOutputPointLayer, QStringLiteral( "utf-8" ), outputPointFields, QgsWkbTypes::Point,
                                         mStrataLayer->crs() );
  if ( outputPointWriter.hasError() != QgsVectorFileWriter::NoError )
  {
    return 3;
  }

  outputPointFields.append( QgsField( QStringLiteral( "bearing" ), QVariant::Double ) ); //add bearing attribute for lines
  QgsVectorFileWriter outputLineWriter( mOutputLineLayer, QStringLiteral( "utf-8" ), outputPointFields, QgsWkbTypes::LineString,
                                        mStrataLayer->crs() );
  if ( outputLineWriter.hasError() != QgsVectorFileWriter::NoError )
  {
    return 4;
  }

  QgsFields usedBaselineFields;
  usedBaselineFields.append( QgsField( QStringLiteral( "stratum_id" ), stratumIdType ) );
  usedBaselineFields.append( QgsField( QStringLiteral( "ok" ), QVariant::String ) );
  QgsVectorFileWriter usedBaselineWriter( mUsedBaselineLayer, QStringLiteral( "utf-8" ), usedBaselineFields, QgsWkbTypes::LineString,
                                          mStrataLayer->crs() );
  if ( usedBaselineWriter.hasError() != QgsVectorFileWriter::NoError )
  {
    return 5;
  }

  //debug: write clipped buffer bounds with stratum id to same directory as out_point
  QFileInfo outputPointInfo( mOutputPointLayer );
  QString bufferClipLineOutput = outputPointInfo.absolutePath() + "/out_buffer_clip_line.shp";
  QgsFields bufferClipLineFields;
  bufferClipLineFields.append( QgsField( QStringLiteral( "id" ), stratumIdType ) );
  QgsVectorFileWriter bufferClipLineWriter( bufferClipLineOutput, QStringLiteral( "utf-8" ), bufferClipLineFields, QgsWkbTypes::LineString, mStrataLayer->crs() );

  //configure distanceArea depending on minDistance units and output CRS
  QgsDistanceArea distanceArea;
  distanceArea.setSourceCrs( mStrataLayer->crs() );
  if ( mMinDistanceUnits == Meters )
  {
    distanceArea.setEllipsoid( QgsProject::instance()->ellipsoid() );
  }
  else
  {
    distanceArea.setEllipsoid( GEO_NONE );
  }

  //possibility to transform output points to lat/long
  QgsCoordinateTransform toLatLongTransform( mStrataLayer->crs(), QgsCoordinateReferenceSystem( 4326, QgsCoordinateReferenceSystem::EpsgCrsId ) );

  //init random number generator
  mt_srand( QTime::currentTime().msec() );

  QgsFeatureRequest fr;
  fr.setSubsetOfAttributes( QStringList() << mStrataIdAttribute << mMinDistanceAttribute << mNPointsAttribute, mStrataLayer->fields() );
  QgsFeatureIterator strataIt = mStrataLayer->getFeatures( fr );

  QgsFeature fet;
  int nTotalTransects = 0;
  int nFeatures = 0;
  int totalFeatures = 0;

  if ( feedback )
  {
    totalFeatures = mStrataLayer->featureCount();
  }

  while ( strataIt.nextFeature( fet ) )
  {
    if ( feedback )
    {
      feedback->setProgress( 100.0 * static_cast< double >( nFeatures ) / totalFeatures );
    }
    if ( feedback && feedback->isCanceled() )
    {
      break;
    }

    if ( !fet.hasGeometry() )
    {
      continue;
    }
    QgsGeometry strataGeom = fet.geometry();

    //find baseline for strata
    QVariant strataId = fet.attribute( mStrataIdAttribute );
    QgsGeometry baselineGeom = findBaselineGeometry( strataId.isValid() ? strataId : -1 );
    if ( baselineGeom.isNull() )
    {
      continue;
    }

    double minDistance = fet.attribute( mMinDistanceAttribute ).toDouble();
    double minDistanceLayerUnits = minDistance;
    //if minDistance is in meters and the data in degrees, we need to apply a rough conversion for the buffer distance
    double bufferDist = bufferDistance( minDistance );
    if ( mMinDistanceUnits == Meters && mStrataLayer->crs().mapUnits() == QgsUnitTypes::DistanceDegrees )
    {
      minDistanceLayerUnits = minDistance / 111319.9;
    }

    QgsGeometry clippedBaseline = strataGeom.intersection( baselineGeom );
    if ( !clippedBaseline || clippedBaseline.wkbType() == QgsWkbTypes::Unknown )
    {
      continue;
    }
    QgsGeometry *bufferLineClipped = clipBufferLine( strataGeom, &clippedBaseline, bufferDist );
    if ( !bufferLineClipped )
    {
      continue;
    }

    //save clipped baseline to file
    QgsFeature blFeature( usedBaselineFields );
    blFeature.setGeometry( clippedBaseline );
    blFeature.setAttribute( QStringLiteral( "stratum_id" ), strataId );
    blFeature.setAttribute( QStringLiteral( "ok" ), "f" );
    usedBaselineWriter.addFeature( blFeature );

    //start loop to create random points along the baseline
    int nTransects = fet.attribute( mNPointsAttribute ).toInt();
    int nCreatedTransects = 0;
    int nIterations = 0;
    int nMaxIterations = nTransects * 50;

    QgsSpatialIndex sIndex; //to check minimum distance
    QMap< QgsFeatureId, QgsGeometry > lineFeatureMap;

    while ( nCreatedTransects < nTransects && nIterations < nMaxIterations )
    {
      double randomPosition = ( ( double )mt_rand() / MD_RAND_MAX ) * clippedBaseline.length();
      QgsGeometry samplePoint = clippedBaseline.interpolate( randomPosition );
      ++nIterations;
      if ( samplePoint.isNull() )
      {
        continue;
      }
      QgsPointXY sampleQgsPointXY = samplePoint.asPoint();
      QgsPointXY latLongSamplePoint = toLatLongTransform.transform( sampleQgsPointXY );

      QgsFeature samplePointFeature( outputPointFields );
      samplePointFeature.setGeometry( samplePoint );
      samplePointFeature.setAttribute( QStringLiteral( "id" ), nTotalTransects + 1 );
      samplePointFeature.setAttribute( QStringLiteral( "station_id" ), nCreatedTransects + 1 );
      samplePointFeature.setAttribute( QStringLiteral( "stratum_id" ), strataId );
      samplePointFeature.setAttribute( QStringLiteral( "station_code" ), strataId.toString() + '_' + QString::number( nCreatedTransects + 1 ) );
      samplePointFeature.setAttribute( QStringLiteral( "start_lat" ), latLongSamplePoint.y() );
      samplePointFeature.setAttribute( QStringLiteral( "start_long" ), latLongSamplePoint.x() );

      //find closest point on clipped buffer line
      QgsPointXY minDistPoint;

      int afterVertex;
      if ( bufferLineClipped->closestSegmentWithContext( sampleQgsPointXY, minDistPoint, afterVertex ) < 0 )
      {
        continue;
      }

      //bearing between sample point and min dist point (transect direction)
      double bearing = distanceArea.bearing( sampleQgsPointXY, minDistPoint ) / M_PI * 180.0;

      QgsPointXY ptFarAway( sampleQgsPointXY.x() + ( minDistPoint.x() - sampleQgsPointXY.x() ) * 1000000,
                            sampleQgsPointXY.y() + ( minDistPoint.y() - sampleQgsPointXY.y() ) * 1000000 );
      QgsPolyline lineFarAway;
      lineFarAway << sampleQgsPointXY << ptFarAway;
      QgsGeometry lineFarAwayGeom = QgsGeometry::fromPolyline( lineFarAway );
      QgsGeometry lineClipStratum = lineFarAwayGeom.intersection( strataGeom );
      if ( lineClipStratum.isNull() )
      {
        continue;
      }

      //cancel if distance between sample point and line is too large (line does not start at point)
      if ( lineClipStratum.distance( samplePoint ) > 0.000001 )
      {
        continue;
      }

      //if lineClipStratum is a multiline, take the part line closest to sampleQgsPoint
      if ( lineClipStratum.wkbType() == QgsWkbTypes::MultiLineString
           || lineClipStratum.wkbType() == QgsWkbTypes::MultiLineString25D )
      {
        QgsGeometry singleLine = closestMultilineElement( sampleQgsPointXY, lineClipStratum );
        if ( !singleLine.isNull() )
        {
          lineClipStratum = singleLine;
        }
      }

      //cancel if length of lineClipStratum is too small
      double transectLength = distanceArea.measureLength( lineClipStratum );
      if ( transectLength < mMinTransectLength )
      {
        continue;
      }

      //search closest existing profile. Cancel if dist < minDist
      if ( otherTransectWithinDistance( lineClipStratum, minDistanceLayerUnits, minDistance, sIndex, lineFeatureMap, distanceArea ) )
      {
        continue;
      }

      QgsFeatureId fid( nCreatedTransects );
      QgsFeature sampleLineFeature( outputPointFields, fid );
      sampleLineFeature.setGeometry( lineClipStratum );
      sampleLineFeature.setAttribute( QStringLiteral( "id" ), nTotalTransects + 1 );
      sampleLineFeature.setAttribute( QStringLiteral( "station_id" ), nCreatedTransects + 1 );
      sampleLineFeature.setAttribute( QStringLiteral( "stratum_id" ), strataId );
      sampleLineFeature.setAttribute( QStringLiteral( "station_code" ), strataId.toString() + '_' + QString::number( nCreatedTransects + 1 ) );
      sampleLineFeature.setAttribute( QStringLiteral( "start_lat" ), latLongSamplePoint.y() );
      sampleLineFeature.setAttribute( QStringLiteral( "start_long" ), latLongSamplePoint.x() );
      sampleLineFeature.setAttribute( QStringLiteral( "bearing" ), bearing );
      outputLineWriter.addFeature( sampleLineFeature );

      //add point to file writer here.
      //It can only be written if the corresponding transect has been as well
      outputPointWriter.addFeature( samplePointFeature );

      sIndex.insertFeature( sampleLineFeature );
      lineFeatureMap.insert( fid, lineClipStratum );

      ++nTotalTransects;
      ++nCreatedTransects;
    }

    QgsFeature bufferClipFeature;
    bufferClipFeature.setGeometry( *bufferLineClipped );
    delete bufferLineClipped;
    bufferClipFeature.setAttribute( QStringLiteral( "id" ), strataId );
    bufferClipLineWriter.addFeature( bufferClipFeature );
    //delete bufferLineClipped;

    ++nFeatures;
  }

  if ( feedback )
  {
    feedback->setProgress( 100.0 );
  }

  return 0;
}

QgsGeometry QgsTransectSample::findBaselineGeometry( const QVariant &strataId )
{
  if ( !mBaselineLayer )
  {
    return QgsGeometry();
  }

  QgsFeatureIterator baseLineIt = mBaselineLayer->getFeatures( QgsFeatureRequest().setSubsetOfAttributes( QStringList( mBaselineStrataId ), mBaselineLayer->fields() ) );
  QgsFeature fet;

  while ( baseLineIt.nextFeature( fet ) ) //todo: cache this in case there are many baslines
  {
    if ( strataId == fet.attribute( mBaselineStrataId ) || mShareBaseline )
    {
      return fet.geometry();
    }
  }
  return QgsGeometry();
}

bool QgsTransectSample::otherTransectWithinDistance( const QgsGeometry &geom, double minDistLayerUnit, double minDistance, QgsSpatialIndex &sIndex,
    const QMap< QgsFeatureId, QgsGeometry > &lineFeatureMap, QgsDistanceArea &da )
{
  if ( geom.isNull() )
  {
    return false;
  }

  QgsGeometry buffer = geom.buffer( minDistLayerUnit, 8 );
  if ( buffer.isNull() )
  {
    return false;
  }
  QgsRectangle rect = buffer.boundingBox();
  QList<QgsFeatureId> lineIdList = sIndex.intersects( rect );

  QList<QgsFeatureId>::const_iterator lineIdIt = lineIdList.constBegin();
  for ( ; lineIdIt != lineIdList.constEnd(); ++lineIdIt )
  {
    const QMap< QgsFeatureId, QgsGeometry >::const_iterator idMapIt = lineFeatureMap.find( *lineIdIt );
    if ( idMapIt != lineFeatureMap.constEnd() )
    {
      double dist = 0;
      QgsPointXY pt1, pt2;
      closestSegmentPoints( geom, idMapIt.value(), dist, pt1, pt2 );
      dist = da.measureLine( pt1, pt2 ); //convert degrees to meters if necessary

      if ( dist < minDistance )
      {
        return true;
      }
    }
  }

  return false;
}

bool QgsTransectSample::closestSegmentPoints( const QgsGeometry &g1, const QgsGeometry &g2, double &dist, QgsPointXY &pt1, QgsPointXY &pt2 )
{
  QgsWkbTypes::Type t1 = g1.wkbType();
  if ( t1 != QgsWkbTypes::LineString && t1 != QgsWkbTypes::LineString25D )
  {
    return false;
  }

  QgsWkbTypes::Type t2 = g2.wkbType();
  if ( t2 != QgsWkbTypes::LineString && t2 != QgsWkbTypes::LineString25D )
  {
    return false;
  }

  QgsPolyline pl1 = g1.asPolyline();
  QgsPolyline pl2 = g2.asPolyline();

  if ( pl1.size() < 2 || pl2.size() < 2 )
  {
    return false;
  }

  QgsPointXY p11 = pl1.at( 0 );
  QgsPointXY p12 = pl1.at( 1 );
  QgsPointXY p21 = pl2.at( 0 );
  QgsPointXY p22 = pl2.at( 1 );

  double p1x = p11.x();
  double p1y = p11.y();
  double v1x = p12.x() - p11.x();
  double v1y = p12.y() - p11.y();
  double p2x = p21.x();
  double p2y = p21.y();
  double v2x = p22.x() - p21.x();
  double v2y = p22.y() - p21.y();

  double denominatorU = v2x * v1y - v2y * v1x;
  double denominatorT = v1x * v2y - v1y * v2x;

  if ( qgsDoubleNear( denominatorU, 0 ) || qgsDoubleNear( denominatorT, 0 ) )
  {
    //lines are parallel
    //project all points on the other segment and take the one with the smallest distance
    QgsPointXY minDistPoint1;
    double d1 = p11.sqrDistToSegment( p21.x(), p21.y(), p22.x(), p22.y(), minDistPoint1 );
    QgsPointXY minDistPoint2;
    double d2 = p12.sqrDistToSegment( p21.x(), p21.y(), p22.x(), p22.y(), minDistPoint2 );
    QgsPointXY minDistPoint3;
    double d3 = p21.sqrDistToSegment( p11.x(), p11.y(), p12.x(), p12.y(), minDistPoint3 );
    QgsPointXY minDistPoint4;
    double d4 = p22.sqrDistToSegment( p11.x(), p11.y(), p12.x(), p12.y(), minDistPoint4 );

    if ( d1 <= d2 && d1 <= d3 && d1 <= d4 )
    {
      dist = std::sqrt( d1 );
      pt1 = p11;
      pt2 = minDistPoint1;
      return true;
    }
    else if ( d2 <= d1 && d2 <= d3 && d2 <= d4 )
    {
      dist = std::sqrt( d2 );
      pt1 = p12;
      pt2 = minDistPoint2;
      return true;
    }
    else if ( d3 <= d1 && d3 <= d2 && d3 <= d4 )
    {
      dist = std::sqrt( d3 );
      pt1 = p21;
      pt2 = minDistPoint3;
      return true;
    }
    else
    {
      dist = std::sqrt( d4 );
      pt1 = p21;
      pt2 = minDistPoint4;
      return true;
    }
  }

  double u = ( p1x * v1y - p1y * v1x - p2x * v1y + p2y * v1x ) / denominatorU;
  double t = ( p2x * v2y - p2y * v2x - p1x * v2y + p1y * v2x ) / denominatorT;

  if ( u >= 0 && u <= 1.0 && t >= 0 && t <= 1.0 )
  {
    dist = 0;
    pt1.setX( p2x + u * v2x );
    pt1.setY( p2y + u * v2y );
    pt2 = pt1;
    dist = 0;
    return true;
  }

  if ( t > 1.0 )
  {
    pt1.setX( p12.x() );
    pt1.setY( p12.y() );
  }
  else if ( t < 0.0 )
  {
    pt1.setX( p11.x() );
    pt1.setY( p11.y() );
  }
  if ( u > 1.0 )
  {
    pt2.setX( p22.x() );
    pt2.setY( p22.y() );
  }
  if ( u < 0.0 )
  {
    pt2.setX( p21.x() );
    pt2.setY( p21.y() );
  }
  if ( t >= 0.0 && t <= 1.0 )
  {
    //project pt2 onto g1
    pt2.sqrDistToSegment( p11.x(), p11.y(), p12.x(), p12.y(), pt1 );
  }
  if ( u >= 0.0 && u <= 1.0 )
  {
    //project pt1 onto g2
    pt1.sqrDistToSegment( p21.x(), p21.y(), p22.x(), p22.y(), pt2 );
  }

  dist = std::sqrt( pt1.sqrDist( pt2 ) );
  return true;
}

QgsGeometry QgsTransectSample::closestMultilineElement( const QgsPointXY &pt, const QgsGeometry &multiLine )
{
  if ( !multiLine || ( multiLine.wkbType() != QgsWkbTypes::MultiLineString
                       && multiLine.wkbType() != QgsWkbTypes::MultiLineString25D ) )
  {
    return QgsGeometry();
  }

  double minDist = DBL_MAX;
  double currentDist = 0;
  QgsGeometry currentLine;
  QgsGeometry closestLine;
  QgsGeometry pointGeom = QgsGeometry::fromPoint( pt );

  QgsMultiPolyline multiPolyline = multiLine.asMultiPolyline();
  QgsMultiPolyline::const_iterator it = multiPolyline.constBegin();
  for ( ; it != multiPolyline.constEnd(); ++it )
  {
    currentLine = QgsGeometry::fromPolyline( *it );
    currentDist = pointGeom.distance( currentLine );
    if ( currentDist < minDist )
    {
      minDist = currentDist;
      closestLine = currentLine;
    }
  }

  return closestLine;
}

QgsGeometry *QgsTransectSample::clipBufferLine( const QgsGeometry &stratumGeom, QgsGeometry *clippedBaseline, double tolerance )
{
  if ( !stratumGeom || !clippedBaseline || clippedBaseline->wkbType() == QgsWkbTypes::Unknown )
  {
    return nullptr;
  }

  QgsGeometry usedBaseline = *clippedBaseline;
  if ( mBaselineSimplificationTolerance >= 0 )
  {
    //int verticesBefore = usedBaseline->asMultiPolyline().count();
    usedBaseline = clippedBaseline->simplify( mBaselineSimplificationTolerance );
    if ( usedBaseline.isNull() )
    {
      return nullptr;
    }
    //int verticesAfter = usedBaseline->asMultiPolyline().count();

    //debug: write to file
    /*QgsVectorFileWriter debugWriter( "/tmp/debug.shp", "utf-8", QgsFields(), QgsWkbTypes::LineString, &( mStrataLayer->crs() ) );
    QgsFeature debugFeature; debugFeature.setGeometry( usedBaseline );
    debugWriter.addFeature( debugFeature );*/
  }

  double currentBufferDist = tolerance;
  int maxLoops = 10;

  for ( int i = 0; i < maxLoops; ++i )
  {
    //loop with tolerance: create buffer, convert buffer to line, clip line by stratum, test if result is (single) line
    QgsGeometry clipBaselineBuffer = usedBaseline.buffer( currentBufferDist, 8 );
    if ( clipBaselineBuffer.isNull() )
    {
      continue;
    }

    //it is also possible that clipBaselineBuffer is a multipolygon
    QgsGeometry bufferLine; //buffer line or multiline
    QgsGeometry bufferLineClipped;
    QgsMultiPolyline mpl;
    if ( clipBaselineBuffer.isMultipart() )
    {
      QgsMultiPolygon bufferMultiPolygon = clipBaselineBuffer.asMultiPolygon();
      if ( bufferMultiPolygon.size() < 1 )
      {
        continue;
      }

      for ( int j = 0; j < bufferMultiPolygon.size(); ++j )
      {
        int size = bufferMultiPolygon.at( j ).size();
        for ( int k = 0; k < size; ++k )
        {
          mpl.append( bufferMultiPolygon.at( j ).at( k ) );
        }
      }
      bufferLine = QgsGeometry::fromMultiPolyline( mpl );
    }
    else
    {
      QgsPolygon bufferPolygon = clipBaselineBuffer.asPolygon();
      if ( bufferPolygon.size() < 1 )
      {
        continue;
      }

      int size = bufferPolygon.size();
      mpl.reserve( size );
      for ( int j = 0; j < size; ++j )
      {
        mpl.append( bufferPolygon[j] );
      }
      bufferLine = QgsGeometry::fromMultiPolyline( mpl );
    }
    bufferLineClipped = bufferLine.intersection( stratumGeom );

    if ( bufferLineClipped.isNull() && bufferLineClipped.type() == QgsWkbTypes::LineGeometry )
    {
      //if stratumGeom is a multipolygon, bufferLineClipped must intersect each part
      bool bufferLineClippedIntersectsStratum = true;
      if ( stratumGeom.wkbType() == QgsWkbTypes::MultiPolygon || stratumGeom.wkbType() == QgsWkbTypes::MultiPolygon25D )
      {
        QVector<QgsPolygon> multiPoly = stratumGeom.asMultiPolygon();
        QVector<QgsPolygon>::const_iterator multiIt = multiPoly.constBegin();
        for ( ; multiIt != multiPoly.constEnd(); ++multiIt )
        {
          QgsGeometry poly = QgsGeometry::fromPolygon( *multiIt );
          if ( !poly.intersects( bufferLineClipped ) )
          {
            bufferLineClippedIntersectsStratum = false;
            break;
          }
        }
      }

      if ( bufferLineClippedIntersectsStratum )
      {
        return new QgsGeometry( bufferLineClipped );
      }
    }

    currentBufferDist /= 2;
  }

  return nullptr; //no solution found even with reduced tolerances
}

double QgsTransectSample::bufferDistance( double minDistanceFromAttribute ) const
{
  double bufferDist = minDistanceFromAttribute;
  if ( mBaselineBufferDistance >= 0 )
  {
    bufferDist = mBaselineBufferDistance;
  }

  if ( mMinDistanceUnits == Meters && mStrataLayer->crs().mapUnits() == QgsUnitTypes::DistanceDegrees )
  {
    bufferDist /= 111319.9;
  }

  return bufferDist;
}
