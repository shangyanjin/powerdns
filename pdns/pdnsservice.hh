/*
    PowerDNS Versatile Database Driven Nameserver
    Copyright (C) 2002  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation
    

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef AHUDNSSERVICE_HH
#define AHUDNSSERVICE_HH

#include <string>
#include "ntservice.hh"

class PDNSService : public NTService
{
protected:
  //! Main service procedure.
  int main( int argc, char *argv[] );

  //! Control handler.
  void ctrlHandler( DWORD controlCode );
  
public:
  //! Constructor.
  PDNSService( void ) : NTService()
  {
  }
  
  //! Returns the service name.
  std::string getServiceName( void );
  
};

#endif // AHUDNSSERVICE_HH
