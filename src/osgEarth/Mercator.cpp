/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/Mercator>
#include <osg/Math>
#include <osg/Notify>
#include <sstream>
#include <algorithm>
#include <string.h>

using namespace osgEarth;

#define PROPERTY_FILTER     "filter"
#define VALUE_FILTER_NEAREST      "nearest"
#define VALUE_FILTER_LINEAR       "linear"

/********************************************************************/

#define MIN_X -20037508.342789244
#define MIN_Y -20037508.342789244
#define MAX_X 20037508.342789244
#define MAX_Y 20037508.342789244

#define PIXELS_PER_TILE 256

void Mercator::metersToLatLon(const double &x, const double &y, double &lat, double &lon)
{
    lon = x / MAX_X * 180.0;
    lat = osg::RadiansToDegrees( 2.0 * atan( exp( (y / MAX_Y) * osg::PI ) ) - .5*osg::PI );

    //Clamp the values to appropriate ranges.
    lon = osg::clampBetween(lon, -180.0, 180.0);
    lat = osg::clampBetween(lat, -90.0, 90.0);
}

void Mercator::latLongToMeters(const double &lat, const double &lon, double &x, double &y)
{
    x = lon * MAX_X / 180.0;
    y = log(tan((90 + lat) * osg::PI / 360)) / (osg::PI / 180.0);
    y = y * MAX_Y / 180.0;

    //Clamp the values to appropriate ranges
    x = osg::clampBetween(x, MIN_X, MAX_X);
    y = osg::clampBetween(y, MIN_Y, MAX_Y);
}

static double
clamp( double n, double min_value, double max_value )
{
    return osg::minimum( osg::maximum( n, min_value ), max_value );
}

int
Mercator::longLatToPixelXY(double lon, double lat, unsigned int lod, int tile_size,
                                  unsigned int& out_x, unsigned int& out_y )
{
    double x = (lon + 180.0) / 360.0;
    double sin_lat = sin( osg::DegreesToRadians( lat ) );
    double y = 0.5 - log( (1+sin_lat) / (1-sin_lat) ) / (4*osg::PI);

    double map_size = (double)TileKey::getMapSizePixels( tile_size, lod );
    
    double raw_x = x * map_size + 0.5;
    double raw_y = y * map_size + 0.5;

    double clamp_x = clamp(raw_x, 0, map_size - 1);
    double clamp_y = clamp(raw_y, 0, map_size - 1);

    out_x = (unsigned int)clamp_x;
    out_y = (unsigned int)clamp_y;

    return raw_y < clamp_y? -1 : raw_y > clamp_y ? 1 : 0;
    //return raw_x == clamp_x && raw_y == clamp_y;
}


void
Mercator::pixelXYtoTileXY(unsigned int x, unsigned int y,
                                 int tile_size,
                                 unsigned int& out_tile_x,
                                 unsigned int& out_tile_y)
{
    out_tile_x = x/tile_size;
    out_tile_y = y/tile_size;
}

/********************************************************************/


struct MercatorTile
{
    MercatorTile() { }
    MercatorTile( TileKey* _key, TileSource* source ) : key( _key )
    {
        image = source->createImage( key.get() );
        key->getPixelExtents( min_x, min_y, max_x, max_y, source->getPixelsPerTile() );
        double dummy;
        key->getGeoExtents( dummy, min_lat, dummy, max_lat );
    }
    osg::ref_ptr<TileKey> key;
    osg::ref_ptr<osg::Image> image;
    unsigned int min_x, min_y, max_x, max_y;
    double min_lat, max_lat;
};


struct MercatorSuperTile : public std::vector<MercatorTile>
{
    MercatorSuperTile(int _tile_size) : tile_size(_tile_size) { }

    int getRow( double lon, double lat, unsigned int lod ) {
        unsigned int x,y;
        if ( lat < (*this)[0].min_lat ) {
            return 0;
        }
        for( int i=0; i<(int)size(); i++ ) {
            if ( lat >= (*this)[i].min_lat && lat <= (*this)[i].max_lat ) {
                Mercator::longLatToPixelXY( lon, lat, lod, tile_size, x, y );
                //osg::notify(osg::NOTICE) << "i=" << i << ", y=" << y << std::endl;
                return i*tile_size + tile_size-(y-(*this)[i].min_y);
            }
        }
        return t()-1;
    }

    unsigned char* data( int s, int t ) {
        return (*this)[t/tile_size].image->data( 0, t%tile_size );
    }

    int t(){ 
        return size()*tile_size;
    }
    double min_lat() { return (*this)[0].min_lat; }
    double max_lat() { return (*this)[size()-1].max_lat; }

private:
    int tile_size;
};

static osg::Image*
sharpen( osg::Image* input )
{
    int filter[9] = { 0, -1, 0, -1, 5, -1, 0, -1, 0 };
    osg::Image* output = new osg::Image( *input );
    for( int t=1; t<input->t()-1; t++ )
    {
        for( int s=1; s<input->s()-1; s++ )
        {
            int pixels[9] = {
                *(int*)input->data(s-1,t-1), *(int*)input->data(s,t-1), *(int*)input->data(s+1,t-1),
                *(int*)input->data(s-1,t  ), *(int*)input->data(s,t  ), *(int*)input->data(s+1,t  ),
                *(int*)input->data(s-1,t+1), *(int*)input->data(s,t+1), *(int*)input->data(s+1,t+1) };

            int shifts[4] = { 0, 8, 16, 32 };

            for( int c=0; c<4; c++ ) // components
            {
                int mask = 0xff << shifts[c];
                int sum = 0;
                for( int i=0; i<9; i++ )
                {
                    sum += ((pixels[i] & mask) >> shifts[c]) * filter[i];
                }
                sum = sum > 255? 255 : sum < 0? 0 : sum;
                output->data(s,t)[c] = sum;
            }
        }
    }
    return output;
}

/********************************************************************/


MercatorToGeodeticTileConverter::MercatorToGeodeticTileConverter(
    TileSource* _source,
    const osgDB::ReaderWriter::Options* options )
{
    filter = MercatorToGeodeticTileConverter::FILTER_NEAREST_NEIGHBOR;
    source = _source;

    if ( options )
    {
        if ( options->getPluginData( PROPERTY_FILTER ) )
        {
            std::string filter = (const char*)options->getPluginData( PROPERTY_FILTER );
            if ( filter == VALUE_FILTER_NEAREST )
                setFilter( MercatorToGeodeticTileConverter::FILTER_NEAREST_NEIGHBOR );
            else if ( filter == VALUE_FILTER_LINEAR )
                setFilter( MercatorToGeodeticTileConverter::FILTER_LINEAR );
        }
    }
}

void
MercatorToGeodeticTileConverter::setFilter( const MercatorToGeodeticTileConverter::Filter& _filter )
{
    filter = _filter;
}

osg::Image*
MercatorToGeodeticTileConverter::createImage( const TileKey* geodetic_key )
{
    if ( !geodetic_key->isGeodetic() )
    {
        osg::notify(osg::WARN) << "[osgEarth] MercatorToGeodeticTileConverter only operates on geodetic TileKeys " << std::endl;
    }

    unsigned int lod = geodetic_key->getLevelOfDetail();

    double dst_min_lon, dst_min_lat, dst_max_lon, dst_max_lat;
    if ( !geodetic_key->getGeoExtents( dst_min_lon, dst_min_lat, dst_max_lon, dst_max_lat ) )
    {
        osg::notify( osg::WARN ) << "GET EXTENTS FAILED!" << std::endl;
        return NULL;
    }

    int tile_size = source->getPixelsPerTile();
    osg::ref_ptr<osg::Image> dst_tile = new osg::Image();
    dst_tile->allocateImage( tile_size, tile_size, 1, GL_RGB, GL_UNSIGNED_BYTE );

    // determine the mercator tiles that overlap the plate carre tile:
    // TODO: actually, no need to worry about X tiles, they will be identical
    unsigned int merc_pixel_min_x, merc_pixel_min_y;
    unsigned int merc_pixel_max_x, merc_pixel_max_y;
    Mercator::longLatToPixelXY( dst_min_lon, dst_min_lat, lod, tile_size, merc_pixel_min_x, merc_pixel_max_y );
    Mercator::longLatToPixelXY( dst_max_lon, dst_max_lat, lod, tile_size, merc_pixel_max_x, merc_pixel_min_y );

    // adjust:
    merc_pixel_max_x--;
    merc_pixel_max_y--;

    unsigned int merc_tile_min_x, merc_tile_min_y;
    Mercator::pixelXYtoTileXY( merc_pixel_min_x, merc_pixel_min_y, tile_size, merc_tile_min_x, merc_tile_min_y );
    unsigned int merc_tile_max_x, merc_tile_max_y;
    Mercator::pixelXYtoTileXY( merc_pixel_max_x, merc_pixel_max_y, tile_size, merc_tile_max_x, merc_tile_max_y );
    
    // collect all the mercator tiles that overlap the destination plate carre tile:
    MercatorSuperTile supertile( tile_size );
    //osg::notify(osg::NOTICE) << "Supertile: " << std::endl;
    for( unsigned int tile_y = merc_tile_min_y; tile_y <= merc_tile_max_y; tile_y++ )
    {
        //MercatorCellKey mqk( merc_tile_min_x, tile_y, lod );
        MercatorTile src_tile( new TileKey( merc_tile_min_x, tile_y, lod, TileGridProfile(TileGridProfile::GLOBAL_MERCATOR)), source.get() );
        if ( src_tile.image.valid() )
        {
            supertile.insert( supertile.begin(), src_tile );
            //osg::notify(osg::NOTICE) << "   tile: min_lat=" << src_tile.min_lat << ", max_lat=" << src_tile.max_lat << std::endl;
        }
    }

    if ( supertile.size() > 0 )
    {
        if ( filter == MercatorToGeodeticTileConverter::FILTER_LINEAR )
        {
            double dst_lat = dst_max_lat;
            double dst_lat_interval = (dst_max_lat-dst_min_lat)/(double)dst_tile->t();

            for( int dst_row = dst_tile->t()-1; dst_row >= 0; dst_row--, dst_lat -= dst_lat_interval )
            {
                int src_row_hi = supertile.getRow( dst_min_lon, dst_lat, lod );
                int src_row_lo = supertile.getRow( dst_min_lon, dst_lat-dst_lat_interval, lod );

                if ( src_row_hi < 0 || src_row_lo < 0 )
                    continue;

                if ( src_row_hi > supertile.t()-1 ) src_row_hi = supertile.t()-1;
                if ( src_row_lo < 0 ) src_row_lo = 0;

                for( int b = 0; b < (int)dst_tile->getRowSizeInBytes(); b++ )
                {
                    int total = 0, r;
                    for( r = src_row_lo; r <= src_row_hi; r++ )
                    {
                        total += (int)(supertile.data( 0, r )[b]);
                    }
                    total /= (src_row_hi-src_row_lo+1);
                    dst_tile->data( 0, dst_row )[b] = (unsigned char)total;
                }
            }
        }

        else if ( filter == MercatorToGeodeticTileConverter::FILTER_NEAREST_NEIGHBOR )
        {
            // loop through all the overlapping mercator tiles, fetch each one, and copy a portion of it
            // into the destination plate carre tile image. (NOTE: tiles will only overlap in the Y 
            // direction; X size will always be equal)
            double dst_lat = dst_max_lat;
            double dst_lat_interval = (dst_max_lat-dst_min_lat)/(double)dst_tile->t();
            for( int dst_row = dst_tile->t()-1; dst_row >= 0; dst_row--, dst_lat -= dst_lat_interval )
            {
                bool copied_line = false;

                // find the src tile and row for dst_lat
                int src_row = supertile.getRow( dst_min_lon, dst_lat, lod );
                 //unsigned int src_row = src_y - src_tile.min_y;

                if ( src_row < 0 ) src_row = 0;
                else if ( src_row > supertile.t()-1 ) src_row = supertile.t()-1;

                memcpy(
                    dst_tile->data( 0, dst_row ),
                    supertile.data( 0, src_row ),
                    dst_tile->getRowSizeInBytes() );
            }
        }
    }

    else
    {
        //No images from the source could be created, return NULL and let the image be deleted
        osg::notify(osg::INFO) << "[osgEarth] MercatorToGeodeticTileConverter:  No images could be created " << std::endl;
        return NULL;
    }

    //Imagery was created successfully from the Source, so return the image
    return dst_tile.release();
}


osg::HeightField*
MercatorToGeodeticTileConverter::createHeightField( const TileKey* pc_key )
{
    //TODO
    osg::notify( osg::WARN ) << "[osgEarth] MercatorToGeodeticTileConverter: heightfields not supported" << std::endl;
    return NULL;
}


/*********************************************************************/


MercatorLocator::MercatorLocator( const osgTerrain::Locator& rhs, int _tile_size, unsigned int _lod ) :
osgTerrain::Locator(rhs), tile_size( _tile_size ), lod(_lod)
{
    //NOP
}

MercatorLocator::MercatorLocator( int _tile_size, unsigned int _lod ) :
tile_size(_tile_size), lod( _lod )
{
    //NOP
}

bool
MercatorLocator::convertLocalToModel(const osg::Vec3d& local, osg::Vec3d& world) const
{
    switch(_coordinateSystemType)
    {
        case(GEOCENTRIC):
        {
            return Locator::convertLocalToModel( local, world );
        }
        case(GEOGRAPHIC):
        {        
            return Locator::convertLocalToModel( local, world );
            //world = local * _transform;

            //double lon_deg = world.x();
            //double lat_deg = world.y();
            //unsigned int px, py;
            //MercatorTileKey::longLatToPixelXY( lon_deg, lat_deg, lod, px, py );
            //if ( py % tile_size != 0 )
            //{
            //    py %= tile_size;
            //    world.y() = ((double)tile_size-(double)py)/(double)tile_size;
            //    //osg::notify(osg::NOTICE) << "lat="<<lat_deg<<", lon="<<lon_deg<<", py="<<py<<", worldy="<<world.y()<<std::endl;
            //}
            return true;      
        }
        case(PROJECTED):
        {        
            return Locator::convertLocalToModel( local, world );
        }
    }    

    return false;
}

bool
MercatorLocator::convertModelToLocal(const osg::Vec3d& world, osg::Vec3d& local) const
{
    // OSG 2.7 bug workaround: bug fix in Locator submitted by GW on 10/3/2008:
    const_cast<MercatorLocator*>(this)->_inverse.invert( _transform );

    switch(_coordinateSystemType)
    {
    case(GEOCENTRIC):
        {        
            double longitude, latitude, height;

            _ellipsoidModel->convertXYZToLatLongHeight(world.x(), world.y(), world.z(),
                latitude, longitude, height );

            local = osg::Vec3d(longitude, latitude, height) * _inverse;

            double lon_deg = osg::RadiansToDegrees(longitude);
            double lat_deg = osg::RadiansToDegrees(latitude);
            unsigned int px, py;
            Mercator::longLatToPixelXY( lon_deg, lat_deg, lod, tile_size, px, py );
            if ( py % tile_size != 0 )
            {
                py %= tile_size;
                local.y() = ((double)tile_size-(double)py)/(double)tile_size;
                //osg::notify(osg::NOTICE) << "KEY="<<key->str()<<": lat="<<lat_deg<<", lon="<<lon_deg<<", py="<<py<<", localy="<<local.y()<<std::endl;
            }

            return true;      
        }
    case(GEOGRAPHIC):
        {        
            local = world * _inverse;

            osg::Vec3d w = world;
            double lon_deg = w.x();
            double lat_deg = w.y();

            unsigned int px, py;
            Mercator::longLatToPixelXY( lon_deg, lat_deg, lod, tile_size, px, py );
            if ( py % tile_size != 0 )
            {
                py %= tile_size;
                local.y() = ((double)tile_size-(double)py)/(double)tile_size;
               // osg::notify(osg::NOTICE) << "lod="<<lod<<", lat="<<lat_deg<<", lon="<<lon_deg<<", py="<<py<<", local.y="<<local.y()<<std::endl;
            }

            return true;      
        }
    case(PROJECTED):
        {        
            local = world * _inverse;
            return true;      
        }
    }    

    return false;
}
