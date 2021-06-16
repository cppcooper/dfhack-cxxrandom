/* Prevent channeling down into known open space.
Author:  Josh Cooper
Created: Aug. 4 2020
Updated: Jun. 15 2021
*/

#include <PluginManager.h>
#include <modules/EventManager.h>
#include <modules/Job.h>

#include "channel-safely.h"

using namespace DFHack;

bool enabled = false;

void onTick(color_ostream &out, void* tick_ptr);
void onStart(color_ostream &out, void* job);
void onComplete(color_ostream &out, void* job);
void manage_designations();

void cancelJob(df::job* job);
bool is_dig(df::job* job);
bool is_dig(df::tile_designation &designation);
bool is_channel(df::job* job);
bool is_channel(df::tile_designation &designation);
bool is_designated(df::tile_designation &designation);
bool is_marked(df::tile_occupancy &occupancy);

command_result manage_channel_designations (color_ostream &out, std::vector <std::string> &parameters);

DFhackCExport command_result plugin_init( color_ostream &out, std::vector<PluginCommand> &commands) {
    commands.push_back(PluginCommand("channel-safely",
                                     "A tool to manage active channel designations.",
                                     manage_channel_designations,
                                     false,
                                     "\n"));
    namespace EM = EventManager;
    using namespace EM::EventType;
    EM::EventHandler tickHandler(onTick, 100);
    EM::EventHandler jobStartHandler(onStart, 0);
    EM::EventHandler jobCompletionHandler(onComplete, 0);
    EM::registerTick(tickHandler, 100, plugin_self);
    EM::registerListener(EventType::JOB_INITIATED, jobStartHandler, plugin_self);
    EM::registerListener(EventType::JOB_COMPLETED, jobCompletionHandler, plugin_self);
    return CR_OK;
}

command_result manage_channel_designations(color_ostream &out, std::vector<std::string> &parameters){
    out.print("m_c_d()\n");
    manage_designations();
    return CR_OK;
}

void onTick(color_ostream &out, void* tick_ptr){
    if(enabled) {
        static int32_t last_tick_counter = 0;
        int32_t tick_counter = (int32_t) ((intptr_t) tick_ptr);
        if ((tick_counter - last_tick_counter) >= 100) {
            last_tick_counter = tick_counter;
            manage_designations();
        }
    }
}

void onStart(color_ostream &out, void* job_ptr){
    if(enabled) {
        df::job* job = (df::job*) job_ptr;
        if (is_dig(job) || is_channel(job)) {
            // channeling from above would be unsafe
            df::coord pos = job->pos;
            pos.z += 1;
            df::tile_designation &aboved = *Maps::getTileDesignation(pos);
            if (is_channel(aboved)) {
                cancelJob(job);
                df::tile_occupancy &job_o = *Maps::getTileOccupancy(pos);
                job_o.bits.dig_marked = true;
            }
        }
    }
}

void onComplete(color_ostream &out, void* job_ptr){
    if(enabled) {
        df::job* job = (df::job*) job_ptr;
        if (is_dig(job) || is_channel(job)) {
            //above isn't safe to channel
            df::coord pos = job->pos;
            pos.z += 1;
            df::tile_designation &aboved = *Maps::getTileDesignation(pos);
            if (is_channel(aboved)) {
                df::tile_occupancy &aboveo = *Maps::getTileOccupancy(pos);
                aboveo.bits.dig_marked = true;
            }
            //below is safe to do anything
            pos.z -= 2;
            df::tile_designation &belowd = *Maps::getTileDesignation(pos);
            if (is_channel(belowd) || is_dig(belowd)) {
                df::tile_occupancy &belowo = *Maps::getTileOccupancy(pos);
                belowo.bits.dig_marked = false;
            }
        }
    }
}

void manage_designations(){
    ChannelManager m;
    m.manage_designations();
}


void ChannelManager::manage_designations() {
    dig_jobs.clear();
    find_dig_jobs();
    foreach_block_column();
}

df::job* ChannelManager::find_job(df::coord &tile_pos) {
    auto iter = dig_jobs.lower_bound(tile_pos.z);
    while (iter != dig_jobs.end()) {
        df::coord &pos = iter->second->pos;
        if (pos == tile_pos) {
            return iter->second;
        }
        iter++;
    }
    return nullptr;
}

void ChannelManager::foreach_block_column() {
    uint32_t x, y, z;
    Maps::getSize(x, y, z);
    for (int ix = 0; ix < x; ++ix) {
        for (int iy = 0; iy < y; ++iy) {
            for (int iz = z - 1; iz > 0; --iz) { //
                df::map_block* top = Maps::getBlock(x, y, iz);
                df::map_block* bottom = Maps::getBlock(x, y, iz - 1);
                if (top->flags.bits.designated &&
                    bottom->flags.bits.designated) {
                    foreach_tile(top, bottom);
                }
            }
        }
    }
}

void ChannelManager::foreach_tile(df::map_block* top, df::map_block* bottom) {
    /** Safety checks
     * is the top tile being channeled or designated to be channeled
     * - Yes
     *      is the bottom tile being dug or designated and not marked
     *      - Yes
     *           try to cancelJob
     *           mark it for later
     * - No
     *      is the bottom tile designated
     *      - Yes
     *           unmark for later
     */
    for (int x = 0; x < 16; ++x) {
        for (int y = 0; y < 16; ++y) {
            auto &top_d = top->designation[x][y];
            auto &top_o = top->occupancy[x][y];
            auto &bottom_d = bottom->designation[x][y];
            auto &bottom_o = bottom->occupancy[x][y];
            df::job* top_job = find_job(top->map_pos);
            df::job* bottom_job = find_job(bottom->map_pos);
            if (top_job || is_channel(top_d)) {
                if (bottom_job || !is_marked(bottom_o) && is_designated(bottom_d)) {
                    cancelJob(bottom_job);
                    bottom_o.bits.dig_marked = true;
                }
            } else if (is_marked(bottom_o) && is_designated(bottom_d)) {
                bottom_o.bits.dig_marked = false;
            }
        }
    }
}

void ChannelManager::find_dig_jobs() {
    df::job_list_link* p = df::global::world->jobs.list.next;
    while (p) {
        df::job* job = p->item;
        p = p->next;
        if (is_dig(job) || is_channel(job)) {
            dig_jobs.emplace(job->pos.z, job);
        }
    }
}

void cancelJob(df::job* job) {
    if(job) {
        df::coord &pos = job->pos;
        df::map_block *job_block = Maps::getTileBlock(pos);
        uint16_t x, y;
        x = pos.x % 16;
        y = pos.y % 16;
        df::tile_designation &designation = job_block->designation[x][y];
        designation.bits.dig = is_channel(job) ?
                               df::tile_dig_designation::Channel : df::tile_dig_designation::Default;
        Job::removeJob(job);
    }
}

bool is_dig(df::job* job) {
    return job->job_type == df::job_type::Dig;
}

bool is_dig(df::tile_designation &designation) {
    return designation.bits.dig == df::tile_dig_designation::Default;
}

bool is_channel(df::job* job){
    return job->job_type == df::job_type::DigChannel;
}

bool is_channel(df::tile_designation &designation){
    return designation.bits.dig == df::tile_dig_designation::Channel;
}

bool is_designated(df::tile_designation &designation){
    return is_dig(designation) || is_channel(designation);
}

bool is_marked(df::tile_occupancy &occupancy){
    return occupancy.bits.dig_marked;
}