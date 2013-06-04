// ttl.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*    Copyright (C) 2013 Tokutek Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"

#include "mongo/db/commands/fsync.h"
#include "mongo/db/ttl.h"
#include "mongo/db/databaseholder.h"
#include "mongo/db/instance.h"
#include "mongo/db/ops/delete.h"
#include "mongo/util/background.h"
#include "mongo/db/replutil.h"

namespace mongo {
    
    class TTLMonitor : public BackgroundJob {
    public:
        TTLMonitor(){}
        virtual ~TTLMonitor(){}

        virtual string name() const { return "TTLMonitor"; }
        
        static string secondsExpireField;
        
        void doTTLForDB( const string& dbName ) {
            Client::GodScope god;

            vector<BSONObj> indexes;
            {
                auto_ptr<DBClientCursor> cursor =
                                db.query( dbName + ".system.indexes" ,
                                          BSON( secondsExpireField << BSON( "$exists" << true ) ) ,
                                          0 , /* default nToReturn */
                                          0 , /* default nToSkip */
                                          0 , /* default fieldsToReturn */
                                          QueryOption_SlaveOk ); /* perform on secondaries too */
                if ( cursor.get() ) {
                    while ( cursor->more() ) {
                        indexes.push_back( cursor->next().getOwned() );
                    }
                }
            }
            
            for ( unsigned i=0; i<indexes.size(); i++ ) {
                BSONObj idx = indexes[i];
                BSONObj key = idx["key"].Obj();
                if ( key.nFields() != 1 ) {
                    error() << "key for ttl index can only have 1 field" << endl;
                    continue;
                }

                BSONObj query;
                {
                    BSONObjBuilder b;
                    b.appendDate( "$lt" , curTimeMillis64() - ( 1000 * idx[secondsExpireField].numberLong() ) );
                    query = BSON( key.firstElement().fieldName() << b.obj() );
                }
                
                LOG(1) << "TTL: " << key << " \t " << query << endl;
                
                long long n = 0;
                {
                    string ns = idx["ns"].String();
                    OpSettings settings;
                    settings.setQueryCursorMode(WRITE_LOCK_CURSOR);
                    cc().setOpSettings(settings);

                    Client::ReadContext ctx(ns);
                    Client::Transaction transaction(DB_SERIALIZABLE);
                    NamespaceDetails* nsd = nsdetails(ns.c_str());
                    if (!nsd) {
                        // collection was dropped
                        continue;
                    }
                    // only do deletes if on master
                    if (!isMasterNs(dbName.c_str())) {
                        continue;
                    }
                    n = deleteObjects(ns.c_str(), query, false, true);
                    transaction.commit();
                }

                LOG(1) << "\tTTL deleted: " << n << endl;
            }
        }

        virtual void run() {
            Client::initThread( name().c_str() );

            while ( ! inShutdown() ) {
                sleepsecs( 60 );

                if ( cmdLine.gdb ) {
                    // Disable TTL while debugging.
                    continue;
                }
                
                LOG(3) << "TTLMonitor thread awake" << endl;
                
                // if part of replSet but not in a readable state (e.g. during initial sync), skip.
                if ( theReplSet && !theReplSet->state().readable() )
                    continue;

                set<string> dbs;
                {
                    Lock::DBRead lk( "local" );
                    dbHolder().getAllShortNames( dbs );
                }
                
                for ( set<string>::const_iterator i=dbs.begin(); i!=dbs.end(); ++i ) {
                    string db = *i;
                    try {
                        doTTLForDB( db );
                    }
                    catch ( DBException& e ) {
                        error() << "error processing ttl for db: " << db << " " << e << endl;
                    }
                }

            }
        }

        DBDirectClient db;
    };

    void startTTLBackgroundJob() {
        TTLMonitor* ttl = new TTLMonitor();
        ttl->go();
    }    
    
    string TTLMonitor::secondsExpireField = "expireAfterSeconds";
}
