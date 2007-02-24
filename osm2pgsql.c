/*
  #-----------------------------------------------------------------------------
  # osm2pgsql - converts planet.osm file into PostgreSQL
  # compatible output suitable to be rendered by mapnik
  # Use: osm2pgsql planet.osm > planet.sql
  #-----------------------------------------------------------------------------
  # Original Python implementation by Artem Pavlenko
  # Re-implementation by Jon Burgess, Copyright 2006
  #
  # This program is free software; you can redistribute it and/or
  # modify it under the terms of the GNU General Public License
  # as published by the Free Software Foundation; either version 2
  # of the License, or (at your option) any later version.
  # 
  # This program is distributed in the hope that it will be useful,
  # but WITHOUT ANY WARRANTY; without even the implied warranty of
  # MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  # GNU General Public License for more details.
  # 
  # You should have received a copy of the GNU General Public License
  # along with this program; if not, write to the Free Software
  # Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
  #-----------------------------------------------------------------------------
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libxml/xmlstring.h>
#include <libxml/xmlreader.h>

#include "bst.h"
#include "avl.h"
#include "build_geometry.h"

#define WKT_MAX 128000
#define SQL_MAX 140000
 	
#if 0
#define DEBUG printf
#else
#define DEBUG(x, ...)
#endif

struct tagDesc {
      const char *name;
      const char *type;
      const int polygon;
}; 

static struct tagDesc exportTags[] = {
   {"name",    "text", 0},
   {"place",   "text", 0},
   {"landuse", "text", 1},
   {"leisure", "text", 1},
   {"natural", "text", 1},
   {"man_made","text", 0},
   {"waterway","text", 0},
   {"highway", "text", 0},
   {"railway", "text", 0},
   {"amenity", "text", 1},
   {"tourism", "text", 0},
   {"learning","text", 0}
};

static const char *table_name = "planet_osm";

#define MAX_ID_NODE (35000000)
#define MAX_ID_SEGMENT (35000000)
 	
struct osmNode {
      unsigned int id;
      double lon;
      double lat;
};
 	
struct osmSegment {
      unsigned int id;
      unsigned int from;
      unsigned int to;
};
 	
struct osmWay {
      unsigned int id;
      char *values;
      char *wkt;     
};

static struct osmNode    nodes[MAX_ID_NODE+1];
static struct osmSegment segments[MAX_ID_SEGMENT+1];

struct bst_table *node_positions;
struct avl_table *segment_unique;
struct avl_table *way_tree;

static int count_node, count_all_node, count_dupe_node;
static int count_segment, count_all_segment, count_dupe_segment;
static int count_way, count_all_way, count_dupe_way;
static int count_way_seg;

// Enable this to suppress duplicate ways in the output
// This is useful on the planet-061128.osm dump and earlier
// to remove lots of redundant data in the US Tiger import.
// Note: This approximately doubles the RAM usage!
static int suppress_dupes=0;

struct keyval {
      char *key;
      char *value;
      struct keyval *next;
      struct keyval *prev;
};
 	

static struct keyval keys, tags, segs;


void usage(const char *arg0)
{
   fprintf(stderr, "Usage error:\n\t%s planet.osm  > planet.sql\n", arg0);
   fprintf(stderr, "or\n\tgzip -dc planet.osm.gz | %s - | gzip -c > planet.sql.gz\n", arg0);
}

void initList(struct keyval *head)
{
   head->next = head;
   head->prev = head;
   head->key = NULL;
   head->value = NULL;
}

void freeItem(struct keyval *p)
{
   free(p->key);
   free(p->value);
   free(p);
}


unsigned int countList(struct keyval *head) 
{
   struct keyval *p = head->next;
   unsigned int count = 0;	

   while(p != head) {
      count++;
      p = p->next;
   }
   return count;
}

int listHasData(struct keyval *head) 
{
   return (head->next != head);
}


char *getItem(struct keyval *head, const char *name)
{
   struct keyval *p = head->next;
   while(p != head) {
      if (!strcmp(p->key, name))
         return p->value;
      p = p->next;
   }
   return NULL;
}	


struct keyval *popItem(struct keyval *head)
{
   struct keyval *p = head->next;
   if (p == head)
      return NULL;

   head->next = p->next;
   p->next->prev = head;

   p->next = NULL;
   p->prev = NULL;

   return p;
}	


void pushItem(struct keyval *head, struct keyval *item)
{
   item->next = head;
   item->prev = head->prev;
   head->prev->next = item;
   head->prev = item;
}	

int addItem(struct keyval *head, const char *name, const char *value, int noDupe)
{
   struct keyval *item;
	
   if (noDupe) {
      item = head->next;
      while (item != head) {
         if (!strcmp(item->value, value) && !strcmp(item->key, name)) {
            //fprintf(stderr, "Discarded %s=%s\n", name, value);
            return 1;
         }
         item = item->next;
      }
   }
	
   item = malloc(sizeof(struct keyval));
		
   if (!item) {
      fprintf(stderr, "Error allocating keyval\n");
      return 2;
   }

   item->key   = strdup(name);
   item->value = strdup(value);

   item->next = head->next;
   item->prev = head;
   head->next->prev = item;
   head->next = item;

   return 0;
}

void resetList(struct keyval *head) 
{
   struct keyval *item;
	
   while((item = popItem(head))) 
      freeItem(item);
}

size_t WKT(int polygon)
{
   while (listHasData(&segs))
   {
      struct keyval *p;
      unsigned int id, to, from;
      double x0, y0, x1, y1;
      p = popItem(&segs);
      id = strtoul(p->value, NULL, 10);

      from = segments[id].from;
      to   = segments[id].to; 

      x0 = nodes[from].lon;
      y0 = nodes[from].lat;
      x1 = nodes[to].lon;
      y1 = nodes[to].lat;
      add_segment(x0,y0,x1,y1);
   }
   return  build_geometry(polygon);
}


void StartElement(xmlTextReaderPtr reader, const xmlChar *name)
{
   xmlChar *xid, *xlat, *xlon, *xfrom, *xto, *xk, *xv;
   unsigned int id, to, from;
   double lon, lat;
   char *k;

   if (xmlStrEqual(name, BAD_CAST "node")) {
      struct osmNode *node, *dupe;
      xid  = xmlTextReaderGetAttribute(reader, BAD_CAST "id");
      xlon = xmlTextReaderGetAttribute(reader, BAD_CAST "lon");
      xlat = xmlTextReaderGetAttribute(reader, BAD_CAST "lat");
      assert(xid); assert(xlon); assert(xlat);
      id  = strtoul((char *)xid, NULL, 10);
      lon = strtod((char *)xlon, NULL);
      lat = strtod((char *)xlat, NULL);

      assert(id > 0 && id < MAX_ID_NODE);
      count_all_node++;
      if (count_all_node%10000 == 0) 
         fprintf(stderr, "\rProcessing: Node(%dk)", count_all_node/1000);

      node = &nodes[id];
      node->id  = id;
      node->lon = lon;
      node->lat = lat;

      dupe = suppress_dupes ? bst_insert(node_positions, (void *)node) : NULL;
		
      if (!dupe) {
         DEBUG("NODE(%d) %f %f\n", id, lon, lat);
      } else {
         node->id = dupe->id;
         count_dupe_node++;
         DEBUG("NODE(%d) %f %f - dupe %d\n", id, lon, lat, dupe->id);
      }
      addItem(&keys, "id", (char *)xid, 0);

      xmlFree(xid);
      xmlFree(xlon);
      xmlFree(xlat);
   } else if (xmlStrEqual(name, BAD_CAST "segment")) {
      xid   = xmlTextReaderGetAttribute(reader, BAD_CAST "id");
      xfrom = xmlTextReaderGetAttribute(reader, BAD_CAST "from");
      xto   = xmlTextReaderGetAttribute(reader, BAD_CAST "to");
      assert(xid); assert(xfrom); assert(xto);
      id   = strtoul((char *)xid, NULL, 10);
      from = strtoul((char *)xfrom, NULL, 10);
      to   = strtoul((char *)xto, NULL, 10);

      assert(id > 0 && id < MAX_ID_SEGMENT);
      if (count_all_segment == 0) {
         //fprintf(stderr, "\nBalancing node tree\n");
         bst_balance(node_positions);
         fprintf(stderr, "\n");
      }

      count_all_segment++;
      if (count_all_segment%10000 == 0) 
         fprintf(stderr, "\rProcessing: Segment(%dk)", count_all_segment/1000);

      if (!nodes[to].id) {
         DEBUG("SEGMENT(%d), NODE(%d) is missing\n", id, to);
      } else if (!nodes[from].id) {
         DEBUG("SEGMENT(%d), NODE(%d) is missing\n", id, from);
      } else {
         from = nodes[from].id;
         to   = nodes[to].id;
         if (from != to) {
            struct osmSegment *segment, *dupe;
            segment = &segments[id];
            segment->id   = id;
            segment->to   = to;
            segment->from = from;

            dupe = suppress_dupes ? avl_insert(segment_unique, (void *)segment) : NULL;

            if (!dupe) {
               count_segment++;
               DEBUG("SEGMENT(%d) %d, %d\n", id, from, to);
            } else {
               count_dupe_segment++;
               segment->id = dupe->id;
               DEBUG("SEGMENT(%d) %d, %d - dupe %d\n", id, from, to, dupe->id);
            }
         }
      }

      xmlFree(xid);
      xmlFree(xfrom);
      xmlFree(xto);
   } else if (xmlStrEqual(name, BAD_CAST "tag")) {
      char *p;
      xk = xmlTextReaderGetAttribute(reader, BAD_CAST "k");
      xv = xmlTextReaderGetAttribute(reader, BAD_CAST "v");
      assert(xk); assert(xv);
      k  = (char *)xmlStrdup(xk);
      /* FIXME: This does not look safe on UTF-8 data */
      while ((p = strchr(k, ':')))
         *p = '_';
      while ((p = strchr(k, ' ')))
         *p = '_';
      addItem(&tags, k, (char *)xv, 0);
      DEBUG("\t%s = %s\n", xk, xv);
      xmlFree(k);
      xmlFree(xk);
      xmlFree(xv);
   } else if (xmlStrEqual(name, BAD_CAST "way")) {
      xid  = xmlTextReaderGetAttribute(reader, BAD_CAST "id");
      assert(xid);
      addItem(&keys, "id", (char *)xid, 0);
      DEBUG("WAY(%s)\n", xid);

      if (count_all_way == 0)
         fprintf(stderr, "\n");
		
      count_all_way++;
      if (count_all_way%1000 == 0) 
         fprintf(stderr, "\rProcessing: Way(%dk)", count_all_way/1000);

      xmlFree(xid);
   } else if (xmlStrEqual(name, BAD_CAST "seg")) {
      xid  = xmlTextReaderGetAttribute(reader, BAD_CAST "id");
      assert(xid);
      id   = strtoul((char *)xid, NULL, 10);
      if (!id || (id > MAX_ID_SEGMENT))
         DEBUG("\tSEG(%s) - invalid segment ID\n", xid);
      else if (!segments[id].id)
         DEBUG("\tSEG(%s) - missing segment\n", xid);
      else {
         char *tmp;
         // Find unique segment
         id = segments[id].id;
         asprintf(&tmp, "%d", id);
         if (addItem(&segs, "id", tmp, 1)) {
            const char *way_id = getItem(&keys, "id");
            if (!way_id) way_id = "???";
            //fprintf(stderr, "Way %s with duplicate segment id %d\n", way_id, id);
            count_way_seg++;
         }
         DEBUG("\tSEG(%s)\n", xid);
         free(tmp);
      }
      xmlFree(xid);
   } else if (xmlStrEqual(name, BAD_CAST "osm")) {
      /* ignore */
   } else {
      fprintf(stderr, "%s: Unknown element name: %s\n", __FUNCTION__, name);
   }
}

void EndElement(xmlTextReaderPtr reader, const xmlChar *name)
{
   unsigned int id;

   DEBUG("%s: %s\n", __FUNCTION__, name);

   if (xmlStrEqual(name, BAD_CAST "node")) {
      int i;
      char *values = NULL, *names = NULL;
      char *osm_id = getItem(&keys, "id");
      if (!osm_id) {
         fprintf(stderr, "%s: Node ID not in keys\n", __FUNCTION__);
         resetList(&keys);
         resetList(&tags);
         return;
      }
      id  = strtoul(osm_id, NULL, 10);
      assert(nodes[id].id);
#if 0
      if (id != nodes[id].id) {
         // TODO: Consider dropping all duplicate nodes or compare tags?
         // Don't really want to store all node attributes for comparison.
         resetList(&keys);
         resetList(&tags);
         return;
      }
#endif
      for (i=0; i < sizeof(exportTags) / sizeof(exportTags[0]); i++) {
         char *v;
         if ((v = getItem(&tags, exportTags[i].name))) {
            if (values) {
               char *oldval = values, *oldnam = names;
               asprintf(&names,  "%s,\"%s\"", oldnam, exportTags[i].name);
               asprintf(&values, "%s,$$%s$$", oldval, v);
               free(oldnam);
               free(oldval);
            } else {
               asprintf(&names,  "\"%s\"", exportTags[i].name);
               asprintf(&values, "$$%s$$", v);
            }
         }
      }
      if (values) {
         char wkt[WKT_MAX];
         count_node++;
         snprintf(wkt, sizeof(wkt)-1, 
                  "POINT(%.15g %.15g)", nodes[id].lon, nodes[id].lat);
         wkt[sizeof(wkt)-1] = '\0';
         printf("insert into %s (osm_id,%s,way) values (%s,%s,GeomFromText('%s',4326));\n", table_name,names,osm_id,values,wkt);
      }
      resetList(&keys);
      resetList(&tags);
      free(values);
      free(names);
   } else if (xmlStrEqual(name, BAD_CAST "segment")) {
      resetList(&tags);
   } else if (xmlStrEqual(name, BAD_CAST "tag")) {
      /* Separate tag list so tag stack unused */
   } else if (xmlStrEqual(name, BAD_CAST "way")) {
      int i, polygon = 0; 
      char *values = NULL, *names = NULL;
      char *osm_id = getItem(&keys, "id");
      
      if (!osm_id) {
         fprintf(stderr, "%s: WAY ID not in keys\n", __FUNCTION__);
         resetList(&keys);
         resetList(&tags);
         resetList(&segs);
         return;
      }

      if (!listHasData(&segs)) {
         DEBUG("%s: WAY(%s) has no segments\n", __FUNCTION__, osm_id);
         resetList(&keys);
         resetList(&tags);
         resetList(&segs);
         return;
      }
      id  = strtoul(osm_id, NULL, 10);

      for (i=0; i < sizeof(exportTags) / sizeof(exportTags[0]); i++) {
         char *v;
         if ((v = getItem(&tags, exportTags[i].name))) {
            if (values) {
               char *oldval = values, *oldnam = names;
               asprintf(&names,  "%s,\"%s\"", oldnam, exportTags[i].name);
               asprintf(&values, "%s,$$%s$$", oldval, v);
               free(oldnam);
               free(oldval);
            } else {
               asprintf(&names,  "\"%s\"", exportTags[i].name);
               asprintf(&values, "$$%s$$", v);
            }
            polygon |= exportTags[i].polygon;
         }
      }      
      if (values) {
         
         size_t wkt_size = WKT(polygon);
         
         if (wkt_size)
         {
            unsigned i;
            for (i=0;i<wkt_size;i++)
            {
               const char * wkt = get_wkt(i);
               if (strlen(wkt)) {
                  struct osmWay *dupe = NULL;	
                  if (suppress_dupes) {
                     struct osmWay *way = malloc(sizeof(struct osmWay));
                     assert(way);
                     way->id = id;
                     way->values = strdup(values);
                     way->wkt    = strdup(wkt);
                     assert(way->values);
                     assert(way->wkt[i]);
                     dupe = avl_insert(way_tree, (void *)way);
                     if (dupe) {
                        DEBUG("WAY(%d) - duplicate of %d\n", id, dupe->id);
                        count_dupe_way++;
                        free(way->values);
                        free(way->wkt);
                        free(way);
                     }
                  } 
                  if (!dupe) {
                     printf("insert into %s (osm_id,%s,way) values (%s,%s,GeomFromText('%s',4326));\n", table_name,names,osm_id,values,wkt);
                     count_way++;	
                  }
               }
            }
            clear_wkts();
         }
      }
      
      resetList(&keys);
      resetList(&tags);
      resetList(&segs);
      free(values);
      free(names);
   } else if (xmlStrEqual(name, BAD_CAST "seg")) {
      /* ignore */
   } else if (xmlStrEqual(name, BAD_CAST "osm")) {
      /* ignore */
   } else {
      fprintf(stderr, "%s: Unknown element name: %s\n", __FUNCTION__, name);
   }
}

static void processNode(xmlTextReaderPtr reader) {
   xmlChar *name;
   name = xmlTextReaderName(reader);
   if (name == NULL)
      name = xmlStrdup(BAD_CAST "--");
	
   switch(xmlTextReaderNodeType(reader)) {
      case XML_READER_TYPE_ELEMENT:
         StartElement(reader, name);	
         if (xmlTextReaderIsEmptyElement(reader))
            EndElement(reader, name); /* No end_element for self closing tags! */
         break;
      case XML_READER_TYPE_END_ELEMENT:
         EndElement(reader, name);
         break;
      case XML_READER_TYPE_SIGNIFICANT_WHITESPACE:
         /* Ignore */
         break;
      default:
         fprintf(stderr, "Unknown node type %d\n", xmlTextReaderNodeType(reader));
         break;
   }
	
   xmlFree(name);
}

void streamFile(char *filename) {
   xmlTextReaderPtr reader;
   int ret;
	
   reader = xmlNewTextReaderFilename(filename);
   if (reader != NULL) {
      ret = xmlTextReaderRead(reader);
      while (ret == 1) {
         processNode(reader);
         ret = xmlTextReaderRead(reader);
      }
	
      if (ret != 0) {
         fprintf(stderr, "%s : failed to parse\n", filename);
         return;
      }
	
      xmlFreeTextReader(reader);
   } else {
      fprintf(stderr, "Unable to open %s\n", filename);
   }
}

int compare_node(const void *bst_a, const void *bst_b, void *bst_param)
{
   const struct osmNode *nA = bst_a;
   const struct osmNode *nB = bst_b;

   if (nA == nB) return 0;
   if (nA->id == nB->id) return 0;

   if (nA->lon < nB->lon)
      return -1;
   else if (nA->lon > nB->lon)
      return +1;
	
   if (nA->lat < nB->lat)
      return -1;
   else if (nA->lat > nB->lat)
      return +1;

   return 0; 
}

int compare_segment(const void *avl_a, const void *avl_b, void *avl_param)
{
   const struct osmSegment *sA = avl_a;
   const struct osmSegment *sB = avl_b;

   if (sA == sB) return 0;
   if (sA->id == sB->id) return 0;

   if (sA->from < sB->from)
      return -1;
   else if (sA->from > sB->from)
      return +1;

   if (sA->to < sB->to)
      return -1;
   else if (sA->to > sB->to)
      return +1;
   return 0;
}

int compare_way(const void *avl_a, const void *avl_b, void *avl_param)
{
   const struct osmWay *wA = avl_a;
   const struct osmWay *wB = avl_b;
   int c;

   if (wA == wB) return 0;
   if (wA->id == wB->id) return 0;

   // TODO: Maybe keeping a hash of WKT would be better?
   c = strcmp(wA->wkt, wB->wkt);
   if (c) return c;

   return strcmp(wA->values, wB->values);
}


int main(int argc, char *argv[])
{
   int i;
        
   if (argc != 2) {
      usage(argv[0]);
      exit(1);
   }
 
   node_positions = bst_create(compare_node, NULL, NULL);
   assert(node_positions);
   segment_unique = avl_create(compare_segment, NULL, NULL);
   assert(segment_unique);
   way_tree = avl_create(compare_way, NULL, NULL);
   assert(way_tree);

   initList(&keys);
   initList(&tags);
   initList(&segs);

   LIBXML_TEST_VERSION
	
   printf("drop table %s ;\n", table_name);
   printf("create table %s ( osm_id int4",table_name);
   for (i=0; i < sizeof(exportTags) / sizeof(exportTags[0]); i++)
      printf(",\"%s\" %s", exportTags[i].name, exportTags[i].type);
   printf(" );\n");
   printf("select AddGeometryColumn('%s', 'way', 4326, 'GEOMETRY', 2 );\n", table_name);
   printf("begin;\n");
	
   streamFile(argv[1]);
	
   printf("commit;\n");
   printf("vacuum analyze %s;\n", table_name);
   printf("CREATE INDEX way_index ON %s USING GIST (way GIST_GEOMETRY_OPS);\n", table_name);
   printf("ALTER TABLE %s ALTER COLUMN way SET NOT NULL;\n",table_name);
   printf("CLUSTER way_index on %s;\n",table_name);
   printf("vacuum analyze %s;\n", table_name);
   
   xmlCleanupParser();
   xmlMemoryDump();
	
   fprintf(stderr, "\n");
	
   if (count_all_node) {
      fprintf(stderr, "Node stats: out(%d), dupe(%d) (%.1f%%), total(%d)\n",
              count_node, count_dupe_node, 100.0 * count_dupe_node / count_all_node, count_all_node);
   }
   if (count_all_segment) {
      fprintf(stderr, "Segment stats: out(%d), dupe(%d) (%.1f%%), total(%d)\n",
              count_segment, count_dupe_segment, 100.0 * count_dupe_segment / count_all_segment, count_all_segment);
   }
   if (count_all_way) {
      fprintf(stderr, "Way stats: out(%d), dupe(%d) (%.1f%%), total(%d)\n",
              count_way, count_dupe_way, 100.0 * count_dupe_way / count_all_way, count_all_way);
   }
   fprintf(stderr, "Way stats: duplicate segments in ways %d\n", count_way_seg);

   return 0;
}