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
        COUT( dirPath );
        return std::vector<uint8_t>();
        /*
        const std::string & filePath = System::concatPath( dirPath, "heroes.icn" );
        std::ifstream instream( filePath, std::ios::in | std::ios::binary );
        std::vector<uint8_t> v( ( std::istreambuf_iterator<char>( instream ) ), std::istreambuf_iterator<char>() );
        return v;
        */
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
