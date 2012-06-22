/*
 * Copyright 2012 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *      Martin Preisler <mpreisle@redhat.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "public/ds.h"
#include "oscap.h"
#include "alloc.h"
#include "common/_error.h"
#include "common/util.h"

#include <sys/stat.h>
#include <libgen.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <string.h>
#include <text.h>

#ifndef MAXPATHLEN
#   define MAXPATHLEN 1024
#endif

static const char* datastream_ns_uri = "http://scap.nist.gov/schema/scap/source/1.2";

static int mkdir_p(const char* path)
{
    if (strlen(path) > MAXPATHLEN)
    {
        return 1;
    }
    else
    {
        char temp[MAXPATHLEN + 1]; // +1 for \0
        unsigned int i;

        for (i = 0; i <= strlen(path); i++)
        {
            if (path[i] == '/' || path[i] == '\0')
            {
                strncpy(temp, path, i);
                temp[i] = '\0';
                mkdir(temp, S_IRWXU);
            }
        }

        return 0;
    }
}

static xmlNodePtr node_get_child_element(xmlNodePtr parent, const char* name)
{
    xmlNodePtr candidate = parent->children;

    for (; candidate != NULL; candidate = candidate->next)
    {
        if (candidate->type != XML_ELEMENT_NODE)
            continue;

        if (name != NULL && strcmp((const char*)(candidate->name), name) != 0)
            continue;

        return candidate;
    }

    return NULL;
}

static xmlNodePtr ds_ids_find_component_ref(xmlDocPtr doc, xmlNodePtr datastream, const char* id)
{
    xmlNodePtr cref_parent = datastream->children;

    for (; cref_parent != NULL; cref_parent = cref_parent->next)
    {
        if (cref_parent->type != XML_ELEMENT_NODE)
            continue;

        xmlNodePtr component_ref = cref_parent->children;

        for (; component_ref != NULL; component_ref = component_ref->next)
        {
            if (component_ref->type != XML_ELEMENT_NODE)
                continue;

            if (strcmp((const char*)(component_ref->name), "component-ref") != 0)
                continue;

            const char* cref_id = (const char*)xmlGetProp(component_ref, (const xmlChar*)"id");
            if (strcmp(cref_id, id) == 0)
            {
                return component_ref;
            }
        }
    }

    return NULL;
}

static void ds_ids_dump_component(const char* component_id, xmlDocPtr doc, const char* filename)
{
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr component = NULL;
    xmlNodePtr candidate = root->children;

    for (; candidate != NULL; candidate = candidate->next)
    {
        if (candidate->type != XML_ELEMENT_NODE)
            continue;

        if (strcmp((const char*)(candidate->name), "component") != 0)
            continue;

        char* candidate_id = (char*)xmlGetProp(candidate, (const xmlChar*)"id");
        if (strcmp(candidate_id, component_id) == 0)
        {
            component = candidate;
            xmlFree(candidate_id);
            break;
        }
        xmlFree(candidate_id);
    }

    if (component == NULL)
    {
        const char* error = oscap_sprintf("Component of given id '%s' was not found in the document.", component_id);
        oscap_seterr(OSCAP_EFAMILY_XML, 0, error);
        oscap_free(error);
        return;
    }

    xmlNodePtr inner_root = node_get_child_element(component, NULL);

    if (inner_root == NULL)
    {
        const char* error = oscap_sprintf("Found component (id='%s') but it has no element contents, nothing to dump, skipping...", component_id);
        oscap_seterr(OSCAP_EFAMILY_XML, 0, error);
        oscap_free(error);
        return;
    }

    xmlDOMWrapCtxtPtr wrap_ctxt = xmlDOMWrapNewCtxt();

    xmlDocPtr new_doc = xmlNewDoc((const xmlChar*)"1.0");
    xmlNodePtr res_node = NULL;
    xmlDOMWrapCloneNode(wrap_ctxt, doc, inner_root, &res_node, new_doc, NULL, 1, 0);
    xmlDocSetRootElement(new_doc, res_node);
    xmlDOMWrapReconcileNamespaces(wrap_ctxt, res_node, 0);
    xmlSaveFileEnc(filename, new_doc, "utf-8");
    xmlFreeDoc(new_doc);

    xmlDOMWrapFreeCtxt(wrap_ctxt);
}

static void ds_ids_dump_component_ref_as(xmlNodePtr component_ref, xmlDocPtr doc, xmlNodePtr datastream, const char* target_dir, const char* filename)
{
    char* cref_id = (char*)xmlGetProp(component_ref, (const xmlChar*)"id");
    if (!cref_id)
    {
        oscap_seterr(OSCAP_EFAMILY_XML, 0, "No or invalid id attribute on given component-ref.");
        xmlFree(cref_id);
        return;
    }

    char* xlink_href = (char*)xmlGetProp(component_ref, (const xmlChar*)"href");
    if (!xlink_href || strlen(xlink_href) < 2)
    {
        oscap_seterr(OSCAP_EFAMILY_XML, 0, "No or invalid xlink:href attribute on given component-ref.");
        xmlFree(xlink_href);
        return;
    }

    const char* component_id = xlink_href + 1 * sizeof(char);
    char* filename_cpy = oscap_sprintf("./%s", filename);
    char* file_reldir = dirname(filename_cpy);

    // the cast is safe to do because we are using the GNU basename, it doesn't
    // modify the string
    const char* file_basename = basename((char*)filename);

    const char* target_filename_dirname = oscap_sprintf("%s/%s", target_dir, file_reldir);
    // TODO: error checking
    mkdir_p(target_filename_dirname);

    const char* target_filename = oscap_sprintf("%s/%s/%s", target_dir, file_reldir, file_basename);
    ds_ids_dump_component(component_id, doc, target_filename);
    oscap_free(target_filename);
    oscap_free(filename_cpy);

    xmlNodePtr catalog = node_get_child_element(component_ref, "catalog");
    if (catalog)
    {
        xmlNodePtr uri = catalog->children;

        for (; uri != NULL; uri = uri->next)
        {
            if (uri->type != XML_ELEMENT_NODE)
                continue;

            if (strcmp((const char*)(uri->name), "uri") != 0)
                continue;

            char* name = (char*)xmlGetProp(uri, (const xmlChar*)"name");

            if (!name)
            {
                oscap_seterr(OSCAP_EFAMILY_XML, 0, "No or invalid name for a component referenced in the catalog. Skipping...");
                xmlFree(name);
                continue;
            }

            char* str_uri = (char*)xmlGetProp(uri, (const xmlChar*)"uri");

            if (!str_uri || strlen(str_uri) < 2)
            {
                oscap_seterr(OSCAP_EFAMILY_XML, 0, "No or invalid URI for a component referenced in the catalog. Skipping...");
                xmlFree(str_uri);
                continue;
            }

            // the pointer arithmetics simply skips the first character which is '#'
            xmlNodePtr cat_component_ref = ds_ids_find_component_ref(doc, datastream, str_uri + 1 * sizeof(char));
            xmlFree(str_uri);

            if (!cat_component_ref)
            {
                const char* error = oscap_sprintf("component-ref with given id '%s' wasn't found in the document!", str_uri + 1 * sizeof(char));
                oscap_seterr(OSCAP_EFAMILY_XML, 0, error);
                oscap_free(error);
                continue;
            }

            ds_ids_dump_component_ref_as(cat_component_ref, doc, datastream, target_filename_dirname, name);
            xmlFree(name);
        }
    }

    oscap_free(target_filename_dirname);
    xmlFree(cref_id);
    xmlFree(xlink_href);
}

static void ds_ids_dump_component_ref(xmlNodePtr component_ref, xmlDocPtr doc, xmlNodePtr datastream, const char* target_dir)
{
    char* xlink_href = (char*)xmlGetProp(component_ref, (const xmlChar*)"href");
    if (!xlink_href || strlen(xlink_href) < 2)
    {
        oscap_seterr(OSCAP_EFAMILY_XML, 0, "No or invalid xlink:href attribute on given component-ref.");
        xmlFree(xlink_href);
        return;
    }

    ds_ids_dump_component_ref_as(component_ref, doc, datastream, target_dir, xlink_href + 1 * sizeof(char));
    xmlFree(xlink_href);
}

void ds_ids_decompose(const char* input_file, const char* id, const char* target_dir)
{
	xmlDocPtr doc = xmlReadFile(input_file, NULL, 0);

    if (!doc)
    {
        const char* error = oscap_sprintf("Could not read/parse XML of given input file at path '%s'.", input_file);
        oscap_seterr(OSCAP_EFAMILY_XML, xmlGetLastError() ? xmlGetLastError()->code : 0, error);
        oscap_free(error);
        return;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);

    xmlNodePtr datastream = NULL;
    xmlNodePtr candidate_datastream = root->children;

    for (; candidate_datastream != NULL; candidate_datastream = candidate_datastream->next)
    {
        if (candidate_datastream->type != XML_ELEMENT_NODE)
            continue;

        if (strcmp((const char*)(candidate_datastream->name), "data-stream") != 0)
            continue;

        // at this point it is sure to be a <data-stream> element

        char* candidate_id = (char*)xmlGetProp(candidate_datastream, (const xmlChar*)"id");
        if (id == NULL || (candidate_id != NULL && strcmp(id, candidate_id) == 0))
        {
            datastream = candidate_datastream;
            xmlFree(candidate_id);
            break;
        }
        xmlFree(candidate_id);
    }

    if (!datastream)
    {
        const char* error = id ?
            oscap_sprintf("Could not find any datastream of id '%s'", id) :
            oscap_sprintf("Could not find any datastream inside the file");

        oscap_seterr(OSCAP_EFAMILY_XML, 0, error);
        oscap_free(error);
        return;
    }

    xmlNodePtr checklists = node_get_child_element(datastream, "checklists");

    if (!checklists)
    {
        oscap_seterr(OSCAP_EFAMILY_XML, 0, "No checklists element found in the matching datastream.");
        return;
    }

    xmlNodePtr component_ref = checklists->children;

    for (; component_ref != NULL; component_ref = component_ref->next)
    {
        if (component_ref->type != XML_ELEMENT_NODE)
            continue;

        if (strcmp((const char*)(component_ref->name), "component-ref") != 0)
            continue;

        ds_ids_dump_component_ref(component_ref, doc, datastream, strcmp(target_dir, "") == 0 ? "." : target_dir);
    }

    xmlFreeDoc(doc);
}

static bool strendswith(const char* str, const char* suffix)
{
    int str_shift = strlen(str) - strlen(suffix);
    if (str_shift < 0)
        return false;

    return strcmp(str + str_shift * sizeof(char), suffix) == 0;
}

void ds_ids_compose_add_component_with_ref(xmlDocPtr doc, xmlNodePtr datastream, const char* filepath, const char* cref_id)
{
    xmlNsPtr ds_ns = xmlSearchNsByHref(doc, datastream, datastream_ns_uri);
    xmlNodePtr cref = xmlNewNode(ds_ns, "component-ref");

    xmlSetProp(cref, "id", cref_id);

    // FIXME: namespace xlink
    const char* xlink_href = oscap_sprintf("#%s", filepath);
    xmlSetProp(cref, "href", xlink_href);
    oscap_free(xlink_href);

    xmlNodePtr cref_catalog = xmlNewNode(ds_ns, "catalog"); // FIXME: namespace
    xmlAddNode(cref, cref_catalog);

    xmlNodePtr cref_parent;

    if (strendswith(filepath, "-xccdf.xml"))
    {
        cref_parent = node_get_child_element(datastream, "checklists");
        //ds_ids_compose_add_xccdf_dependencies(doc, datastream, filepath, cref_catalog);
    }
    else if (strendswith(filepath, "-oval.xml"))
    {
        cref_parent = node_get_child_element(datastream, "checks");
    }
    else if (strendswith(filepath, "-cpe-oval.xml") || strendswith(filepath, "-cpe-dictionary.xml"))
    {
        cref_parent = node_get_child_element(datastream, "dictionaries");
    }
    else
    {
        cref_parent = node_get_child_element(datastream, "extended-components");
    }

    xmlAddChild(cref_parent, cref);
}

ds_ids_compose_from_xccdf(const char* xccdf_file, const char* target_datastream)
{
    xmlDocPtr doc = xmlNewDoc("utf-8");
    xmlNodePtr root = xmlNewNode(NULL, "data-stream-collection");
    xmlNsPtr ds_ns = xmlNewNs(root, datastream_ns_uri, "ds");
    xmlSetNs(root, ds_ns);

    xmlNodePtr dictionaries = xmlNewNode(ds_ns, "dictionaries");
    xmlAddChild(root, dictionaries);

    xmlNodePtr checklists = xmlNewNode(ds_ns, "checklists");
    xmlAddChild(root, checklists);

    xmlNodePtr checks = xmlNewNode(ds_ns, "checks");
    xmlAddChild(root, checks);

    xmlNodePtr extended_components = xmlNewNode(ds_ns, "extended-components");
    xmlAddChild(root, extended_components);

    ds_ids_add_component_with_ref(doc, xccdf_file, xccdf_file);
}
