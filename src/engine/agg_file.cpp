/***************************************************************************
 *   fheroes2: https://github.com/ihhub/fheroes2                           *
 *   Copyright (C) 2020 - 2022                                             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "agg_file.h"

#include <cstdint>
#include <regex>
#include <string>

#include <dir.h>
#include <logging.h>
#include <system.h>
#include <tools.h>
#include <SDL_surface.h>
#include <SDL2/SDL_image.h>

namespace fheroes2
{
    bool AGGFile::open( const std::string & fileName )
    {
        // Note this function gets called 2 times for each AGG:
        // once for the data files and once for the audio files.
        if ( !_stream.open( fileName, "rb" ) )
            return false;

        const size_t size = _stream.size();
        const size_t count = _stream.getLE16();
        const size_t fileRecordSize = sizeof( uint32_t ) * 3;

        if ( count * ( fileRecordSize + _maxFilenameSize ) >= size )
            return false;

        // Check to see if this agg file has a directory next to it with the same name
        // in which unpacked assets may be placed by the user (for overiding purposes):
        const bool hasExternals = AGGFile::collectExternals( fileName );

        StreamBuf fileEntries = _stream.toStreamBuf( count * fileRecordSize );
        const size_t nameEntriesSize = _maxFilenameSize * count;
        _stream.seek( size - nameEntriesSize );
        StreamBuf nameEntries = _stream.toStreamBuf( nameEntriesSize );

        for ( size_t i = 0; i < count; ++i ) {
            std::string name = nameEntries.toString( _maxFilenameSize );
            fileEntries.getLE32(); // skip CRC (?) part
            const uint32_t fileOffset = fileEntries.getLE32();
            const uint32_t fileSize = fileEntries.getLE32();
            _files.try_emplace( std::move( name ), std::make_pair( fileSize, fileOffset ) );
        }
        if ( _files.size() != count ) {
            _files.clear();
            _externals.clear();
            return false;
        }
        return !_stream.fail();
    }

    std::vector<uint8_t> AGGFile::read( const std::string & fileName )
    {
        auto it = _files.find( fileName );
        if ( it != _files.end() ) {
            const auto & fileParams = it->second;
            if ( fileParams.first > 0 ) {
                // The user's override asset has priority:
                auto externalItem = _externals.find( fileName );
                if ( externalItem != _externals.end() ) {
                    const auto & externalParams = externalItem->second;
                    COUT( "Using the external version of " << fileName );
                    return externalParams.first;
                }

                _stream.seek( fileParams.second );
                return _stream.getRaw( fileParams.first );
            }
        }

        // Note: it's entirely possible to NOT find the required asset, because we first check
        // the extension AGG file and only afterwards check the base AGG file.
        return std::vector<uint8_t>();
    }

    bool AGGFile::collectExternals( const std::string & aggFileName )
    {
        std::string dirPath = aggFileName;
        replaceStringEnding( dirPath, ".AGG", "" );
        if ( System::IsDirectory( dirPath ) ) {
            ListFiles files;
            files.ReadDir( dirPath, "", false, true ); // with allowDirs true
            for ( const std::string & file : files ) {
                if ( System::IsDirectory( file ) ) {
                    std::string name = StringUpper( System::GetBasename( file ) );
                    if ( name == "." || name == ".." ) {
                        continue;
                    }

                    std::vector<uint8_t> externalRaw;
                    const std::string type = StringSplit( name, "." ).back();
                    if ( type == "ICN" ) {
                        externalRaw = AGGFile::spawnIcnFromDir( file );
                    }

                    if ( !externalRaw.empty() ) {
                        _externals.try_emplace( std::move( name ), std::make_pair( externalRaw, true ) );
                    }
                }
            }
        }
        return _externals.size() != 0;
    }

    std::vector<uint8_t> AGGFile::spawnIcnFromDir( const std::string & dirPath )
    {
        std::vector<uint8_t> result;

        ListFiles pngFiles;
        pngFiles.ReadDir( dirPath, "png", false );
        if ( pngFiles.empty() ) {
            return {};
        }
       
        const uint16_t count = static_cast<uint16_t>(pngFiles.size());
        const size_t slotHeaderSize = 13;
        uint32_t currentOffset = 0;

        for ( const std::string & pngFile : pngFiles ) {
            COUT( "pngFile:" << pngFile );

            SDL_Surface * surface = IMG_Load( pngFile.c_str() );
            if ( surface == nullptr ) {
                return {};
            }

            const uint16_t width = static_cast<uint16_t>( surface->w );
            const uint16_t height = static_cast<uint16_t>( surface->h );
            const uint32_t numPixels = width * height * 4;
            const uint32_t pixelDataStartsAt = currentOffset + slotHeaderSize;
            
            // Build slot header
            StreamBuf slotHeader( slotHeaderSize );
            slotHeader.putLE16( 0 ); // TODO: offsetX
            slotHeader.putLE16( 0 ); // TODO: offsetY
            slotHeader.putLE16( width );
            slotHeader.putLE16( height );
            slotHeader.put( 0 ); // animation frames
            slotHeader.putLE32( pixelDataStartsAt ); // offsetData
            
            

            // Append the slot header
            const std::vector<uint8_t> slotHeaderRaw = slotHeader.getRaw();
            result.insert( result.end(), slotHeaderRaw.begin(), slotHeaderRaw.end() );

            result.insert( result.end(), 0xAB );
            result.insert( result.end(), 0xCD );
            result.insert( result.end(), 0xEF );


            // Append the slot pixels
            const std::vector<uint8_t> pixels = AGGFile::getPixelsFromSurface( surface );
            result.insert( result.end(), pixels.begin(), pixels.end() );

            currentOffset = static_cast<uint32_t>( result.size() );
        }

        // Build icon header
        StreamBuf iconHeader( 6 ); // 2 bytes (count) + 4 bytes (size)
        iconHeader.putLE16( count ); // how many slots we have
        iconHeader.putLE32( static_cast<uint32_t>(result.size()) ); // total size of the slots
        
        // Append the slot header
        const std::vector<uint8_t> iconHeaderRaw = iconHeader.getRaw();
        result.insert( result.begin(), iconHeaderRaw.begin(), iconHeaderRaw.end() );

        std::ofstream file( "D:\\fheroes2\\build\\x64\\Debug-SDL2\\data\\HEROES2X\\heroes.icn\\temp.icn", std::ios::binary );
        file.write( reinterpret_cast<const char *>( result.data() ), result.size() );

        return result;
    }

    // TODO cleanup the code
    std::vector<uint8_t> AGGFile::getPixelsFromSurface( SDL_Surface * surface )
    {
        // Convert surface to RGBA format if necessary
        SDL_Surface * convertedSurface = surface;
        if ( surface->format->format != SDL_PIXELFORMAT_RGBA32 ) {
            convertedSurface = SDL_ConvertSurfaceFormat( surface, SDL_PIXELFORMAT_RGBA32, 0 );
            if ( convertedSurface == nullptr ) {
                std::cerr << "Error: Unable to convert surface to RGBA format." << std::endl;
                return {};
            }
        }

        std::vector<uint8_t> pixels;
        pixels.reserve( surface->w * surface->h * 4 ); // Pre-allocate memory for performance

        SDL_LockSurface( convertedSurface );
        uint8_t * pixelData = reinterpret_cast<uint8_t *>( convertedSurface->pixels );

        for ( int y = 0; y < convertedSurface->h; ++y ) {
            for ( int x = 0; x < convertedSurface->w; ++x ) {
                uint8_t r = *pixelData++;
                uint8_t g = *pixelData++;
                uint8_t b = *pixelData++;
                uint8_t a = *pixelData++;
                pixels.push_back( r );
                pixels.push_back( g );
                pixels.push_back( b );
                pixels.push_back( a );
            }
        }

        SDL_UnlockSurface( convertedSurface );

        if ( convertedSurface != surface ) {
            SDL_FreeSurface( convertedSurface );
        }

        return pixels;
    }
}

StreamBase & operator>>( StreamBase & st, fheroes2::ICNHeader & icn )
{
    icn.offsetX = st.getLE16();
    icn.offsetY = st.getLE16();
    icn.width = st.getLE16();
    icn.height = st.getLE16();
    icn.animationFrames = st.get();
    icn.offsetData = st.getLE32();

    return st;
}
