/*
    Copyright (C) 2002-2009 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <cstring>
#include <boost/shared_ptr.hpp>

#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/bundle.h"
#include "ardour/user_bundle.h"
#include "ardour/io_processor.h"
#include "ardour/midi_track.h"
#include "ardour/port.h"
#include "ardour/session.h"
#include "ardour/auditioner.h"

#include "gui_thread.h"
#include "port_group.h"
#include "port_matrix.h"
#include "time_axis_view.h"
#include "public_editor.h"

#include "i18n.h"

using namespace std;
using namespace Gtk;
using namespace ARDOUR;

/** PortGroup constructor.
 * @param n Name.
 */
PortGroup::PortGroup (std::string const & n)
	: name (n)
{

}

PortGroup::~PortGroup()
{
	for (BundleList::iterator i = _bundles.begin(); i != _bundles.end(); ++i) {
		delete *i;
	}
	_bundles.clear ();
}

/** Add a bundle to a group.
 *  @param b Bundle.
 *  @param allow_dups true to allow the group to contain more than one bundle with the same port, otherwise false.
 */
void
PortGroup::add_bundle (boost::shared_ptr<Bundle> b, bool allow_dups)
{
	add_bundle_internal (b, boost::shared_ptr<IO> (), false, Gdk::Color (), allow_dups);
}

/** Add a bundle to a group.
 *  @param b Bundle.
 *  @param io IO whose ports are in the bundle.
 */
void
PortGroup::add_bundle (boost::shared_ptr<Bundle> b, boost::shared_ptr<IO> io)
{
	add_bundle_internal (b, io, false, Gdk::Color (), false);
}

/** Add a bundle to a group.
 *  @param b Bundle.
 *  @param c Colour to represent the bundle with.
 */
void
PortGroup::add_bundle (boost::shared_ptr<Bundle> b, boost::shared_ptr<IO> io, Gdk::Color c)
{
	add_bundle_internal (b, io, true, c, false);
}

PortGroup::BundleRecord::BundleRecord (boost::shared_ptr<ARDOUR::Bundle> b, boost::shared_ptr<ARDOUR::IO> iop, Gdk::Color c, bool has_c)
	: bundle (b)
	, io (iop)
	, colour (c)
	, has_colour (has_c)
{
}

void
PortGroup::add_bundle_internal (boost::shared_ptr<Bundle> b, boost::shared_ptr<IO> io, bool has_colour, Gdk::Color colour, bool allow_dups)
{
	assert (b.get());

	if (!allow_dups) {
		
		/* don't add this bundle if we already have one with the same ports */
		
		BundleList::iterator i = _bundles.begin ();
		while (i != _bundles.end() && b->has_same_ports ((*i)->bundle) == false) {
			++i;
		}
		
		if (i != _bundles.end ()) {
			return;
		}
	}

	BundleRecord* br = new BundleRecord (b, io, colour, has_colour);
	b->Changed.connect (br->changed_connection, invalidator (*this), ui_bind (&PortGroup::bundle_changed, this, _1), gui_context());
	_bundles.push_back (br);

	Changed ();	
}

void
PortGroup::remove_bundle (boost::shared_ptr<Bundle> b)
{
	assert (b.get());

	BundleList::iterator i = _bundles.begin ();
	while (i != _bundles.end() && (*i)->bundle != b) {
		++i;
	}

	if (i == _bundles.end()) {
		return;
	}

	delete *i;
	_bundles.erase (i);

	Changed ();
}

void
PortGroup::bundle_changed (Bundle::Change c)
{
	BundleChanged (c);
}


void
PortGroup::clear ()
{
	for (BundleList::iterator i = _bundles.begin(); i != _bundles.end(); ++i) {
		delete *i;
	}

	_bundles.clear ();
	Changed ();
}

bool
PortGroup::has_port (std::string const& p) const
{
	for (BundleList::const_iterator i = _bundles.begin(); i != _bundles.end(); ++i) {
		if ((*i)->bundle->offers_port_alone (p)) {
			return true;
		}
	}

	return false;
}

boost::shared_ptr<Bundle>
PortGroup::only_bundle ()
{
	assert (_bundles.size() == 1);
	return _bundles.front()->bundle;
}


ChanCount
PortGroup::total_channels () const
{
	ChanCount n;
	for (BundleList::const_iterator i = _bundles.begin(); i != _bundles.end(); ++i) {
		n += (*i)->bundle->nchannels ();
	}

	return n;
}

boost::shared_ptr<IO>
PortGroup::io_from_bundle (boost::shared_ptr<ARDOUR::Bundle> b) const
{
	BundleList::const_iterator i = _bundles.begin ();
	while (i != _bundles.end() && (*i)->bundle != b) {
		++i;
	}

	if (i == _bundles.end()) {
		return boost::shared_ptr<IO> ();
	}

	return (*i)->io;
}

/** Remove bundles whose channels are already represented by other, larger bundles */
void
PortGroup::remove_duplicates ()
{
	BundleList::iterator i = _bundles.begin();
	while (i != _bundles.end()) {

		BundleList::iterator tmp = i;
		++tmp;

		bool remove = false;

		for (BundleList::iterator j = _bundles.begin(); j != _bundles.end(); ++j) {

			if ((*j)->bundle->nchannels() > (*i)->bundle->nchannels()) {
				/* this bundle is larger */

				uint32_t k = 0;
				while (k < (*i)->bundle->nchannels().n_total()) {
					/* see if this channel on *i has an equivalent on *j */
					uint32_t l = 0;
					while (l < (*j)->bundle->nchannels().n_total() && (*i)->bundle->channel_ports (k) != (*j)->bundle->channel_ports (l)) {
						++l;
					}

					if (l == (*j)->bundle->nchannels().n_total()) {
						/* it does not */
						break;
					}

					++k;
				}

				if (k == (*i)->bundle->nchannels().n_total()) {
					/* all channels on *i are represented by the larger bundle *j, so remove *i */
					remove = true;
					break;
				}
			}
		}
		
		if (remove) {
			_bundles.erase (i);
		}
		
		i = tmp;
	}
}


/** PortGroupList constructor.
 */
PortGroupList::PortGroupList ()
	: _signals_suspended (false), _pending_change (false), _pending_bundle_change ((Bundle::Change) 0)
{

}

PortGroupList::~PortGroupList() 
{
	/* XXX need to clean up bundles, but ownership shared with PortGroups */
}

void
PortGroupList::maybe_add_processor_to_list (
	boost::weak_ptr<Processor> wp, list<boost::shared_ptr<IO> >* route_ios, bool inputs, set<boost::shared_ptr<IO> >& used_io
	)
{
	boost::shared_ptr<Processor> p (wp.lock());

	if (!p) {
		return;
	}

	boost::shared_ptr<IOProcessor> iop = boost::dynamic_pointer_cast<IOProcessor> (p);

	if (iop) {

		boost::shared_ptr<IO> io = inputs ? iop->input() : iop->output();

		if (io && used_io.find (io) == used_io.end()) {
			route_ios->push_back (io);
			used_io.insert (io);
		}
	}
}

struct RouteIOs {
	RouteIOs (boost::shared_ptr<Route> r, boost::shared_ptr<IO> i) {
		route = r;
		ios.push_back (i);
	}
	
	boost::shared_ptr<Route> route;
	std::list<boost::shared_ptr<IO> > ios;
};

class RouteIOsComparator {
public:
	bool operator() (RouteIOs const & a, RouteIOs const & b) {
		return a.route->order_key (X_("editor")) < b.route->order_key (X_("editor"));
	}
};

/** Gather ports from around the system and put them in this PortGroupList.
 *  @param type Type of ports to collect, or NIL for all types.
 */
void
PortGroupList::gather (ARDOUR::Session* session, ARDOUR::DataType type, bool inputs, bool allow_dups)
{
	clear ();

	if (session == 0) {
		return;
	}

	boost::shared_ptr<PortGroup> bus (new PortGroup (_("Bus")));
	boost::shared_ptr<PortGroup> track (new PortGroup (_("Track")));
	boost::shared_ptr<PortGroup> system (new PortGroup (_("System")));
	boost::shared_ptr<PortGroup> ardour (new PortGroup (_("Ardour")));
	boost::shared_ptr<PortGroup> other (new PortGroup (_("Other")));

	/* Find the IOs which have bundles for routes and their processors.  We store
	   these IOs in a RouteIOs class so that we can then sort the results by route
	   order key.
	*/

	boost::shared_ptr<RouteList> routes = session->get_routes ();
	list<RouteIOs> route_ios;

	for (RouteList::const_iterator i = routes->begin(); i != routes->end(); ++i) {

		/* keep track of IOs that we have taken bundles from,
		   so that we can avoid taking the same IO from both
		   Route::output() and the main_outs Delivery */

		set<boost::shared_ptr<IO> > used_io;
		boost::shared_ptr<IO> io = inputs ? (*i)->input() : (*i)->output();
		used_io.insert (io);

		RouteIOs rb (*i, io);
		(*i)->foreach_processor (boost::bind (&PortGroupList::maybe_add_processor_to_list, this, _1, &rb.ios, inputs, used_io));

		route_ios.push_back (rb);
	}

	/* Sort RouteIOs by the routes' editor order keys */
	route_ios.sort (RouteIOsComparator ());

	/* Now put the bundles that belong to these sorted RouteIOs into the PortGroup.
	   Note that if the RouteIO's bundles are multi-type, we may make new Bundles
	   with only the ports of one type.
	*/
	
	for (list<RouteIOs>::iterator i = route_ios.begin(); i != route_ios.end(); ++i) {
		TimeAxisView* tv = PublicEditor::instance().axis_view_from_route (i->route);

		/* Work out which group to put these IOs' bundles in */
		boost::shared_ptr<PortGroup> g;
		if (boost::dynamic_pointer_cast<Track> (i->route)) {
			g = track;
		} else {
			g = bus;
		}

		for (list<boost::shared_ptr<IO> >::iterator j = i->ios.begin(); j != i->ios.end(); ++j) {
			boost::shared_ptr<Bundle> b = bundle_for_type ((*j)->bundle(), type);
			if (b) {
				if (tv) {
					g->add_bundle (b, *j, tv->color ());
				} else {
					g->add_bundle (b, *j);
				}
			}
		}
	}

	/* Bundles owned by the session; add user bundles first, then normal ones, so
	   that UserBundles that offer the same ports as a normal bundle get priority
	*/

	boost::shared_ptr<BundleList> b = session->bundles ();

	for (BundleList::iterator i = b->begin(); i != b->end(); ++i) {
		if (boost::dynamic_pointer_cast<UserBundle> (*i) && (*i)->ports_are_inputs() == inputs) {
			boost::shared_ptr<Bundle> b = bundle_for_type (*i, type);
			if (b) {
				system->add_bundle (b, allow_dups);
			}
		}
	}

	for (BundleList::iterator i = b->begin(); i != b->end(); ++i) {
		if (boost::dynamic_pointer_cast<UserBundle> (*i) == 0 && (*i)->ports_are_inputs() == inputs) {
			boost::shared_ptr<Bundle> b = bundle_for_type (*i, type);
			if (b) {
				system->add_bundle (b, allow_dups);
			}
		}
	}
	
	/* Ardour stuff */

	if (!inputs) {
		boost::shared_ptr<Bundle> b = bundle_for_type (session->the_auditioner()->output()->bundle(), type);
		if (b) {
			ardour->add_bundle (b);
		}

		b = bundle_for_type (session->click_io()->bundle(), type);
		if (b) {
			ardour->add_bundle (b);
		}
	}

	/* Now find all other ports that we haven't thought of yet */

	std::vector<std::string> extra_system[DataType::num_types];
	std::vector<std::string> extra_other[DataType::num_types];

	const char ** ports = 0;
	if (type == DataType::NIL) {
		ports = session->engine().get_ports ("", "", inputs ? JackPortIsInput : JackPortIsOutput);
	} else {
		ports = session->engine().get_ports ("", type.to_jack_type(), inputs ? JackPortIsInput : JackPortIsOutput);
	}
	
 	if (ports) {

		int n = 0;

		while (ports[n]) {

			std::string const p = ports[n];

			if (!system->has_port(p) &&
			    !bus->has_port(p) &&
			    !track->has_port(p) &&
			    !ardour->has_port(p) &&
			    !other->has_port(p)) {
                                
                                /* special hack: ignore MIDI ports labelled Midi-Through. these
                                   are basically useless and mess things up for default
                                   connections.
                                */

                                if (p.find ("MIDI-Through") != string::npos) {
                                        ++n;
                                        continue;
                                }

				/* can't use the audio engine for this as we are looking at non-Ardour ports */

				jack_port_t* jp = jack_port_by_name (session->engine().jack(), p.c_str());
				if (jp) {
					DataType t (jack_port_type (jp));
					if (t != DataType::NIL) {
						if (port_has_prefix (p, "system:") || port_has_prefix (p, "alsa_pcm") || port_has_prefix (p, "ardour:")) {
							extra_system[t].push_back (p);
						} else {
							extra_other[t].push_back (p);
						}
					}
				}
			}

			++n;
		}

		free (ports);
	}

	for (DataType::iterator i = DataType::begin(); i != DataType::end(); ++i) {
		if (!extra_system[*i].empty()) {
			boost::shared_ptr<Bundle> b = make_bundle_from_ports (extra_system[*i], *i, inputs);
			boost::shared_ptr<Bundle> bt = bundle_for_type (b, type);
			if (bt) {
				system->add_bundle (bt);
			}
		}
	}

	for (DataType::iterator i = DataType::begin(); i != DataType::end(); ++i) {
		if (!extra_other[*i].empty()) {
			boost::shared_ptr<Bundle> b = make_bundle_from_ports (extra_other[*i], *i, inputs);
			boost::shared_ptr<Bundle> bt = bundle_for_type (b, type);
			if (bt) {
				other->add_bundle (bt);
			}
		}
	}

	if (!allow_dups) {
		system->remove_duplicates ();
	}

	add_group_if_not_empty (other);
	add_group_if_not_empty (bus);
	add_group_if_not_empty (track);
	add_group_if_not_empty (ardour);
	add_group_if_not_empty (system);

	emit_changed ();
}

boost::shared_ptr<Bundle>
PortGroupList::make_bundle_from_ports (std::vector<std::string> const & p, ARDOUR::DataType type, bool inputs) const
{
	boost::shared_ptr<Bundle> b (new Bundle ("", inputs));

	std::string const pre = common_prefix (p);
	if (!pre.empty()) {
		b->set_name (pre.substr (0, pre.length() - 1));
	}

	for (uint32_t j = 0; j < p.size(); ++j) {
		b->add_channel (p[j].substr (pre.length()), type);
		b->set_port (j, p[j]);
	}

	return b;
}

bool
PortGroupList::port_has_prefix (const std::string& n, const std::string& p) const
{
	return n.substr (0, p.length()) == p;
}

std::string
PortGroupList::common_prefix_before (std::vector<std::string> const & p, std::string const & s) const
{
	/* we must have some strings and the first must contain the separator string */
	if (p.empty() || p[0].find_first_of (s) == std::string::npos) {
		return "";
	}

	/* prefix of the first string */
	std::string const fp = p[0].substr (0, p[0].find_first_of (s) + 1);

	/* see if the other strings also start with fp */
	uint32_t j = 1;
	while (j < p.size()) {
		if (p[j].substr (0, fp.length()) != fp) {
			break;
		}
		++j;
	}

	if (j != p.size()) {
		return "";
	}

	return fp;
}


std::string
PortGroupList::common_prefix (std::vector<std::string> const & p) const
{
	/* common prefix before '/' ? */
	std::string cp = common_prefix_before (p, "/");
	if (!cp.empty()) {
		return cp;
	}

	cp = common_prefix_before (p, ":");
	if (!cp.empty()) {
		return cp;
	}

	return "";
}

void
PortGroupList::clear ()
{
	_groups.clear ();
	_bundle_changed_connections.drop_connections ();
	emit_changed ();
}


PortGroup::BundleList const &
PortGroupList::bundles () const
{
	_bundles.clear ();

	for (PortGroupList::List::const_iterator i = begin (); i != end (); ++i) {
		std::copy ((*i)->bundles().begin(), (*i)->bundles().end(), std::back_inserter (_bundles));
	}

	return _bundles;
}

ChanCount
PortGroupList::total_channels () const
{
	ChanCount n;

	for (PortGroupList::List::const_iterator i = begin(); i != end(); ++i) {
		n += (*i)->total_channels ();
	}

	return n;
}

void
PortGroupList::add_group_if_not_empty (boost::shared_ptr<PortGroup> g)
{
	if (!g->bundles().empty ()) {
		add_group (g);
	}
}

void
PortGroupList::add_group (boost::shared_ptr<PortGroup> g)
{
	_groups.push_back (g);

	g->Changed.connect (_changed_connections, invalidator (*this), boost::bind (&PortGroupList::emit_changed, this), gui_context());
	g->BundleChanged.connect (_bundle_changed_connections, invalidator (*this), ui_bind (&PortGroupList::emit_bundle_changed, this, _1), gui_context());

	emit_changed ();
}

void
PortGroupList::remove_bundle (boost::shared_ptr<Bundle> b)
{
	for (List::iterator i = _groups.begin(); i != _groups.end(); ++i) {
		(*i)->remove_bundle (b);
	}

	emit_changed ();
}

void
PortGroupList::emit_changed ()
{
	if (_signals_suspended) {
		_pending_change = true;
	} else {
		Changed ();
	}
}

void
PortGroupList::emit_bundle_changed (Bundle::Change c)
{
	if (_signals_suspended) {
		_pending_bundle_change = c;
	} else {
		BundleChanged (c);
	}
}
void
PortGroupList::suspend_signals ()
{
	_signals_suspended = true;
}

void
PortGroupList::resume_signals ()
{
	if (_pending_change) {
		Changed ();
		_pending_change = false;
	}

	if (_pending_bundle_change != 0) {
		BundleChanged (_pending_bundle_change);
		_pending_bundle_change = (ARDOUR::Bundle::Change) 0;
	}

	_signals_suspended = false;
}

boost::shared_ptr<IO>
PortGroupList::io_from_bundle (boost::shared_ptr<ARDOUR::Bundle> b) const
{
	List::const_iterator i = _groups.begin ();
	while (i != _groups.end()) {
		boost::shared_ptr<IO> io = (*i)->io_from_bundle (b);
		if (io) {
			return io;
		}
		++i;
	}

	return boost::shared_ptr<IO> ();
}

bool
PortGroupList::empty () const
{
	List::const_iterator i = _groups.begin ();
	while (i != _groups.end() && (*i)->total_channels() == ChanCount::ZERO) {
		++i;
	}

	return (i == _groups.end());
}

/** Take a bundle, and either return it, if it contains only ports of type \a t,
 *  or return a new bundle with those ports from \a b which are of type \a t.
 *  Note that t == NIL is taken to mean "all types".
 */
boost::shared_ptr<Bundle>
PortGroupList::bundle_for_type (boost::shared_ptr<Bundle> b, DataType t) const
{
	/* We are asked for a bundle with all types, so that's easy */
	if (t == DataType::NIL) {
		return b;
	}

	if (b->nchannels().get(t) == b->nchannels().n_total()) {
		/* All channels on b are of the correct type, so just return b */
		return b;
	}

	/* We must build a new bundle */
	boost::shared_ptr<Bundle> n;
	for (uint32_t i = 0; i < b->nchannels().n_total(); ++i) {
		if (b->channel_type(i) == t) {
			n->add_channel (b->channel_name (i), t, b->channel_ports (i));
		}
	}

	return n;
}
