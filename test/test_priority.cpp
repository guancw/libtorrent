/*

Copyright (c) 2008-2013, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/session.hpp"
#include "libtorrent/session_settings.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/time.hpp"
#include "libtorrent/aux_/path.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/read_resume_data.hpp"
#include "libtorrent/write_resume_data.hpp"
#include <tuple>
#include <functional>

#include "test.hpp"
#include "test_utils.hpp"
#include "setup_transfer.hpp"
#include "settings.hpp"
#include <fstream>
#include <iostream>

using namespace lt;
using std::ignore;

namespace {

alert_category_t const mask = alert::all_categories & ~(alert::performance_warning | alert::stats_notification);

int peer_disconnects = 0;

bool on_alert(alert const* a)
{
	if (alert_cast<peer_disconnected_alert>(a))
		++peer_disconnects;
	else if (alert_cast<peer_error_alert>(a))
		++peer_disconnects;

	return false;
}

void cleanup()
{
	error_code ec;
	remove_all("tmp1_priority", ec);
	remove_all("tmp2_priority", ec);
	remove_all("tmp1_priority_moved", ec);
	remove_all("tmp2_priority_moved", ec);
}

void test_transfer(settings_pack const& sett, bool test_deprecated = false)
{
	// this allows shutting down the sessions in parallel
	std::vector<session_proxy> sp;

	cleanup();

	settings_pack pack = sett;

	// we need a short reconnect time since we
	// finish the torrent and then restart it
	// immediately to complete the second half.
	// using a reconnect time > 0 will just add
	// to the time it will take to complete the test
	pack.set_int(settings_pack::min_reconnect_time, 0);

	pack.set_bool(settings_pack::enable_outgoing_utp, false);
	pack.set_bool(settings_pack::enable_incoming_utp, false);
	pack.set_int(settings_pack::alert_mask, mask);

	pack.set_int(settings_pack::out_enc_policy, settings_pack::pe_disabled);
	pack.set_int(settings_pack::in_enc_policy, settings_pack::pe_disabled);
	pack.set_bool(settings_pack::allow_multiple_connections_per_ip, false);
	pack.set_int(settings_pack::unchoke_slots_limit, 8);

	pack.set_bool(settings_pack::enable_upnp, false);
	pack.set_bool(settings_pack::enable_natpmp, false);
	pack.set_bool(settings_pack::enable_lsd, false);
	pack.set_bool(settings_pack::enable_dht, false);

	pack.set_str(settings_pack::listen_interfaces, "0.0.0.0:48075");
	pack.set_int(settings_pack::alert_mask, mask);
#ifndef TORRENT_NO_DEPRECATE
	pack.set_bool(settings_pack::rate_limit_utp, true);
#endif

	lt::session ses1(pack);

	pack.set_str(settings_pack::listen_interfaces, "0.0.0.0:49075");
	pack.set_int(settings_pack::alert_mask, mask);
	lt::session ses2(pack);

	torrent_handle tor1;
	torrent_handle tor2;

	error_code ec;
	create_directory("tmp1_priority", ec);
	std::ofstream file("tmp1_priority/temporary");
	std::shared_ptr<torrent_info> t = ::create_torrent(&file, "temporary", 16 * 1024, 13, false);
	file.close();

	wait_for_listen(ses1, "ses1");
	wait_for_listen(ses2, "ses2");

	peer_disconnects = 0;

	// test using piece sizes smaller than 16kB
	std::tie(tor1, tor2, ignore) = setup_transfer(&ses1, &ses2, nullptr
		, true, false, true, "_priority", 8 * 1024, &t, false, nullptr);

	int const num_pieces = tor2.torrent_file()->num_pieces();
	aux::vector<download_priority_t, piece_index_t> priorities(std::size_t(num_pieces), 1_pri);
	// set half of the pieces to priority 0
	std::fill(priorities.begin(), priorities.begin() + (num_pieces / 2), 0_pri);
	tor2.prioritize_pieces(priorities);
	std::cout << "setting priorities: ";
	std::copy(priorities.begin(), priorities.end(), std::ostream_iterator<download_priority_t>(std::cout, ", "));
	std::cout << std::endl;

	for (int i = 0; i < 200; ++i)
	{
		print_alerts(ses1, "ses1", true, true, &on_alert);
		print_alerts(ses2, "ses2", true, true, &on_alert);

		torrent_status st1 = tor1.status();
		torrent_status st2 = tor2.status();

		if (i % 10 == 0)
		{
			print_ses_rate(i / 10.f, &st1, &st2);
		}

		// st2 is finished when we have downloaded half of the pieces
		if (st2.is_finished) break;

		if (st2.state != torrent_status::downloading)
		{
			static char const* state_str[] =
				{"checking (q)", "checking", "dl metadata"
				, "downloading", "finished", "seeding", "allocating", "checking (r)"};
			std::cout << "st2 state: " << state_str[st2.state] << std::endl;
		}

		TEST_CHECK(st1.state == torrent_status::seeding
			|| st1.state == torrent_status::checking_resume_data
			|| st1.state == torrent_status::checking_files);
		TEST_CHECK(st2.state == torrent_status::downloading
			|| st2.state == torrent_status::checking_resume_data);

		if (peer_disconnects >= 2) break;

		// if nothing is being transferred after 2 seconds, we're failing the test
		if (st1.upload_payload_rate == 0 && i > 20) break;

		std::this_thread::sleep_for(lt::milliseconds(100));
	}

	TEST_CHECK(!tor2.status().is_seeding);
	TEST_CHECK(tor2.status().is_finished);

	if (tor2.status().is_finished)
		std::cout << "torrent is finished (50% complete)" << std::endl;
	else return;

	std::vector<download_priority_t> priorities2 = tor2.get_piece_priorities();
	std::copy(priorities2.begin(), priorities2.end()
		, std::ostream_iterator<download_priority_t>(std::cout, ", "));
	std::cout << std::endl;
	TEST_CHECK(std::equal(priorities.begin(), priorities.end(), priorities2.begin()));

	std::cout << "force recheck" << std::endl;
	tor2.force_recheck();

	priorities2 = tor2.get_piece_priorities();
	std::copy(priorities2.begin(), priorities2.end()
		, std::ostream_iterator<download_priority_t>(std::cout, ", "));
	std::cout << std::endl;
	TEST_CHECK(std::equal(priorities.begin(), priorities.end(), priorities2.begin()));

	peer_disconnects = 0;

	// when we're done checking, we're likely to be put in downloading state
	// for a split second before transitioning to finished. This loop waits
	// for the finished state
	torrent_status st2;
	for (int i = 0; i < 50; ++i)
	{
		print_alerts(ses1, "ses1", true, true, &on_alert);
		print_alerts(ses2, "ses2", true, true, &on_alert);

		st2 = tor2.status();
		if (i % 10 == 0)
		{
			std::cout << int(st2.progress * 100) << "% " << std::endl;
		}
		if (st2.state == torrent_status::finished) break;
		std::this_thread::sleep_for(lt::milliseconds(100));
	}

	TEST_EQUAL(st2.state, torrent_status::finished);

	if (st2.state != torrent_status::finished)
		return;

	std::cout << "recheck complete" << std::endl;

	priorities2 = tor2.get_piece_priorities();
	std::copy(priorities2.begin(), priorities2.end(), std::ostream_iterator<download_priority_t>(std::cout, ", "));
	std::cout << std::endl;
	TEST_CHECK(std::equal(priorities.begin(), priorities.end(), priorities2.begin()));

	tor2.pause();
	wait_for_alert(ses2, torrent_paused_alert::alert_type, "ses2");

	std::printf("save resume data\n");
	tor2.save_resume_data();

	std::vector<char> resume_data;

	time_point start = clock_type::now();
	while (true)
	{
		ses2.wait_for_alert(seconds(10));
		std::vector<alert*> alerts;
		ses2.pop_alerts(&alerts);
		if (alerts.empty()) break;
		for (std::vector<alert*>::iterator i = alerts.begin()
			, end(alerts.end()); i != end; ++i)
		{
			alert* a = *i;
			std::cout << "ses2: " << a->message() << std::endl;
			if (alert_cast<save_resume_data_alert>(a))
			{
				resume_data = write_resume_data_buf(alert_cast<save_resume_data_alert>(a)->params);
				std::printf("saved resume data\n");
				goto done;
			}
			else if (alert_cast<save_resume_data_failed_alert>(a))
			{
				std::printf("save resume failed\n");
				goto done;
			}
			if (total_seconds(clock_type::now() - start) > 10)
				goto done;
		}
	}
done:
	TEST_CHECK(resume_data.size());

	if (resume_data.empty())
		return;

	std::printf("%s\n", &resume_data[0]);

	ses2.remove_torrent(tor2);

	std::cout << "removed" << std::endl;

	std::this_thread::sleep_for(lt::milliseconds(100));

	std::cout << "re-adding" << std::endl;
	add_torrent_params p;
#ifndef TORRENT_NO_DEPRECATE
	if (test_deprecated)
	{
		p.resume_data = resume_data;
	}
	else
#endif
	{
		error_code ec;
		p = read_resume_data(resume_data, ec);
		TEST_CHECK(!ec);
	}
	p.flags &= ~torrent_flags::paused;
	p.flags &= ~torrent_flags::auto_managed;
	p.ti = t;
	p.save_path = "tmp2_priority";

	tor2 = ses2.add_torrent(p, ec);
	tor2.prioritize_pieces(priorities);
	std::cout << "resetting priorities" << std::endl;
	tor2.resume();

	// wait for torrent 2 to settle in back to finished state (it will
	// start as checking)
	torrent_status st1;
	for (int i = 0; i < 5; ++i)
	{
		print_alerts(ses1, "ses1", true, true, &on_alert);
		print_alerts(ses2, "ses2", true, true, &on_alert);

		st1 = tor1.status();
		st2 = tor2.status();

		TEST_CHECK(st1.state == torrent_status::seeding);

		if (st2.is_finished) break;

		std::this_thread::sleep_for(lt::milliseconds(100));
	}

	// torrent 2 should not be seeding yet, it should
	// just be 50% finished
	TEST_CHECK(!st2.is_seeding);
	TEST_CHECK(st2.is_finished);

	std::fill(priorities.begin(), priorities.end(), 1_pri);
	tor2.prioritize_pieces(priorities);
	std::cout << "setting priorities to 1" << std::endl;
	TEST_EQUAL(tor2.status().is_finished, false);

	std::copy(priorities.begin(), priorities.end()
		, std::ostream_iterator<download_priority_t>(std::cout, ", "));
	std::cout << std::endl;

	// drain alerts
	print_alerts(ses1, "ses1", true, true, &on_alert);
	print_alerts(ses2, "ses2", true, true, &on_alert);

	peer_disconnects = 0;

	// this loop makes sure ses2 reconnects to the peer now that it's
	// in download mode again. If this fails, the reconnect logic may
	// not work or be inefficient
	st1 = tor1.status();
	st2 = tor2.status();
	for (int i = 0; i < 130; ++i)
	{
		print_alerts(ses1, "ses1", true, true, &on_alert);
		print_alerts(ses2, "ses2", true, true, &on_alert);

		st1 = tor1.status();
		st2 = tor2.status();

		if (i % 10 == 0)
			print_ses_rate(i / 10.f, &st1, &st2);

		if (st2.is_seeding) break;

		TEST_EQUAL(st1.state, torrent_status::seeding);
		TEST_EQUAL(st2.state, torrent_status::downloading);

		if (peer_disconnects >= 2)
		{
			std::printf("too many disconnects (%d), exiting\n", peer_disconnects);
			break;
		}

		std::this_thread::sleep_for(lt::milliseconds(100));
	}

	st2 = tor2.status();
	if (!st2.is_seeding)
		std::printf("ses2 failed to reconnect to ses1!\n");
	TEST_CHECK(st2.is_seeding);

	sp.push_back(ses1.abort());
	sp.push_back(ses2.abort());
}

} // anonymous namespace

TORRENT_TEST(priority)
{
	using namespace lt;
	settings_pack p = settings();
	test_transfer(p);
	cleanup();
}

#ifndef TORRENT_NO_DEPRECATE
TORRENT_TEST(priority_deprecated)
{
	using namespace lt;
	settings_pack p;
	test_transfer(p, true);
	cleanup();
}
#endif

// test to set piece and file priority on a torrent that doesn't have metadata
// yet
TORRENT_TEST(no_metadata_file_prio)
{
	settings_pack pack = settings();
	lt::session ses(pack);

	add_torrent_params addp;
	addp.flags &= ~torrent_flags::paused;
	addp.flags &= ~torrent_flags::auto_managed;
	addp.info_hash = sha1_hash("abababababababababab");
	addp.save_path = ".";
	torrent_handle h = ses.add_torrent(addp);

	h.file_priority(file_index_t(0), 0_pri);
	TEST_EQUAL(h.file_priority(file_index_t(0)), 0_pri);
	h.file_priority(file_index_t(0), 1_pri);
	TEST_EQUAL(h.file_priority(file_index_t(0)), 1_pri);

	ses.remove_torrent(h);
}

TORRENT_TEST(no_metadata_piece_prio)
{
	settings_pack pack = settings();
	lt::session ses(pack);

	add_torrent_params addp;
	addp.flags &= ~torrent_flags::paused;
	addp.flags &= ~torrent_flags::auto_managed;
	addp.info_hash = sha1_hash("abababababababababab");
	addp.save_path = ".";
	torrent_handle h = ses.add_torrent(addp);

	// you can't set piece priorities before the metadata has been downloaded
	h.piece_priority(piece_index_t(2), 0_pri);
	TEST_EQUAL(h.piece_priority(piece_index_t(2)), 4_pri);
	h.piece_priority(piece_index_t(2), 1_pri);
	TEST_EQUAL(h.piece_priority(piece_index_t(2)), 4_pri);

	ses.remove_torrent(h);
}
