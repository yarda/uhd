//
// Copyright 2010 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "usrp2_impl.hpp"
#include "usrp2_regs.hpp"
#include <uhd/usrp/mboard_props.hpp>
#include <uhd/utils/assert.hpp>
#include <uhd/types/mac_addr.hpp>
#include <uhd/types/dict.hpp>

using namespace uhd;
using namespace uhd::usrp;

/***********************************************************************
 * Helper Methods
 **********************************************************************/
void usrp2_impl::mboard_init(void){
    _mboards[""] = wax_obj_proxy::make(
        boost::bind(&usrp2_impl::mboard_get, this, _1, _2),
        boost::bind(&usrp2_impl::mboard_set, this, _1, _2)
    );

    //set the time on the usrp2 as close as possible to the system utc time
    boost::posix_time::ptime now(boost::posix_time::microsec_clock::universal_time());
    set_time_spec(time_spec_t(now, get_master_clock_freq()), true);
}

void usrp2_impl::init_clock_config(void){
    //setup the clock configuration settings
    _clock_config.ref_source = clock_config_t::REF_INT;
    _clock_config.pps_source = clock_config_t::PPS_SMA;
    _clock_config.pps_polarity = clock_config_t::PPS_NEG;

    //update the clock config (sends a control packet)
    update_clock_config();
}

void usrp2_impl::update_clock_config(void){
    boost::uint32_t pps_flags = 0;

    //translate pps source enums
    switch(_clock_config.pps_source){
    case clock_config_t::PPS_SMA:  pps_flags |= FRF_TIME64_PPS_SMA;  break;
    case clock_config_t::PPS_MIMO: pps_flags |= FRF_TIME64_PPS_MIMO; break;
    default: throw std::runtime_error("usrp2: unhandled clock configuration pps source");
    }

    //translate pps polarity enums
    switch(_clock_config.pps_polarity){
    case clock_config_t::PPS_POS: pps_flags |= FRF_TIME64_PPS_POSEDGE; break;
    case clock_config_t::PPS_NEG: pps_flags |= FRF_TIME64_PPS_NEGEDGE; break;
    default: throw std::runtime_error("usrp2: unhandled clock configuration pps polarity");
    }

    //set the pps flags
    this->poke(FR_TIME64_FLAGS, pps_flags);

    //TODO clock source ref 10mhz (spi ad9510)
}

void usrp2_impl::set_time_spec(const time_spec_t &time_spec, bool now){
    //set ticks and seconds
    this->poke(FR_TIME64_SECS, time_spec.secs);
    this->poke(FR_TIME64_TICKS, time_spec.ticks);

    //set the register to latch it all in
    boost::uint32_t imm_flags = (now)? FRF_TIME64_LATCH_NOW : FRF_TIME64_LATCH_NEXT_PPS;
    this->poke(FR_TIME64_IMM, imm_flags);
}

/***********************************************************************
 * MBoard Get Properties
 **********************************************************************/
void usrp2_impl::mboard_get(const wax::obj &key_, wax::obj &val){
    wax::obj key; std::string name;
    boost::tie(key, name) = extract_named_prop(key_);

    //handle the other props
    if (key.type() == typeid(std::string)){
        if (key.as<std::string>() == "mac-addr"){
            //setup the out data
            usrp2_ctrl_data_t out_data;
            out_data.id = htonl(USRP2_CTRL_ID_GIVE_ME_YOUR_MAC_ADDR_BRO);

            //send and recv
            usrp2_ctrl_data_t in_data = ctrl_send_and_recv(out_data);
            ASSERT_THROW(htonl(in_data.id) == USRP2_CTRL_ID_THIS_IS_MY_MAC_ADDR_DUDE);

            //extract the address
            val = mac_addr_t::from_bytes(in_data.data.mac_addr).to_string();
            return;
        }

        if (key.as<std::string>() == "ip-addr"){
            //setup the out data
            usrp2_ctrl_data_t out_data;
            out_data.id = htonl(USRP2_CTRL_ID_GIVE_ME_YOUR_IP_ADDR_BRO);

            //send and recv
            usrp2_ctrl_data_t in_data = ctrl_send_and_recv(out_data);
            ASSERT_THROW(htonl(in_data.id) == USRP2_CTRL_ID_THIS_IS_MY_IP_ADDR_DUDE);

            //extract the address
            val = boost::asio::ip::address_v4(ntohl(in_data.data.ip_addr)).to_string();
            return;
        }
    }

    //handle the get request conditioned on the key
    switch(key.as<mboard_prop_t>()){
    case MBOARD_PROP_NAME:
        val = std::string("usrp2 mboard");
        return;

    case MBOARD_PROP_OTHERS:{
            prop_names_t others = boost::assign::list_of
                ("mac-addr")
                ("ip-addr")
            ;
            val = others;
        }
        return;

    case MBOARD_PROP_RX_DBOARD:
        ASSERT_THROW(_rx_dboards.has_key(name));
        val = _rx_dboards[name]->get_link();
        return;

    case MBOARD_PROP_RX_DBOARD_NAMES:
        val = prop_names_t(_rx_dboards.get_keys());
        return;

    case MBOARD_PROP_TX_DBOARD:
        ASSERT_THROW(_tx_dboards.has_key(name));
        val = _tx_dboards[name]->get_link();
        return;

    case MBOARD_PROP_TX_DBOARD_NAMES:
        val = prop_names_t(_tx_dboards.get_keys());
        return;

    case MBOARD_PROP_CLOCK_RATE:
        val = double(get_master_clock_freq());
        return;

    case MBOARD_PROP_RX_DSP:
        ASSERT_THROW(_rx_dsps.has_key(name));
        val = _rx_dsps[name]->get_link();
        return;

    case MBOARD_PROP_RX_DSP_NAMES:
        val = prop_names_t(_rx_dsps.get_keys());
        return;

    case MBOARD_PROP_TX_DSP:
        ASSERT_THROW(_tx_dsps.has_key(name));
        val = _tx_dsps[name]->get_link();
        return;

    case MBOARD_PROP_TX_DSP_NAMES:
        val = prop_names_t(_tx_dsps.get_keys());
        return;

    case MBOARD_PROP_CLOCK_CONFIG:
        val = _clock_config;
        return;

    case MBOARD_PROP_TIME_NOW:
    case MBOARD_PROP_TIME_NEXT_PPS:
        throw std::runtime_error("Error: trying to get write-only property on usrp2 mboard");

    }
}

/***********************************************************************
 * MBoard Set Properties
 **********************************************************************/
void usrp2_impl::mboard_set(const wax::obj &key, const wax::obj &val){
    //handle the other props
    if (key.type() == typeid(std::string)){
        if (key.as<std::string>() == "mac-addr"){
            //setup the out data
            usrp2_ctrl_data_t out_data;
            out_data.id = htonl(USRP2_CTRL_ID_HERE_IS_A_NEW_MAC_ADDR_BRO);
            mac_addr_t mac_addr = mac_addr_t::from_string(val.as<std::string>());
            std::copy(mac_addr.to_bytes(), mac_addr.to_bytes()+mac_addr_t::hlen, out_data.data.mac_addr);

            //send and recv
            usrp2_ctrl_data_t in_data = ctrl_send_and_recv(out_data);
            ASSERT_THROW(htonl(in_data.id) == USRP2_CTRL_ID_THIS_IS_MY_MAC_ADDR_DUDE);
            return;
        }

        if (key.as<std::string>() == "ip-addr"){
            //setup the out data
            usrp2_ctrl_data_t out_data;
            out_data.id = htonl(USRP2_CTRL_ID_HERE_IS_A_NEW_IP_ADDR_BRO);
            out_data.data.ip_addr = htonl(boost::asio::ip::address_v4::from_string(val.as<std::string>()).to_ulong());

            //send and recv
            usrp2_ctrl_data_t in_data = ctrl_send_and_recv(out_data);
            ASSERT_THROW(htonl(in_data.id) == USRP2_CTRL_ID_THIS_IS_MY_IP_ADDR_DUDE);
            return;
        }
    }

    //handle the get request conditioned on the key
    switch(key.as<mboard_prop_t>()){

    case MBOARD_PROP_CLOCK_CONFIG:
        _clock_config = val.as<clock_config_t>();
        update_clock_config();
        return;

    case MBOARD_PROP_TIME_NOW:{
        set_time_spec(val.as<time_spec_t>(), true);
        return;
    }

    case MBOARD_PROP_TIME_NEXT_PPS:{
        set_time_spec(val.as<time_spec_t>(), false);
        return;
    }

    case MBOARD_PROP_NAME:
    case MBOARD_PROP_OTHERS:
    case MBOARD_PROP_CLOCK_RATE:
    case MBOARD_PROP_RX_DSP:
    case MBOARD_PROP_RX_DSP_NAMES:
    case MBOARD_PROP_TX_DSP:
    case MBOARD_PROP_TX_DSP_NAMES:
    case MBOARD_PROP_RX_DBOARD:
    case MBOARD_PROP_RX_DBOARD_NAMES:
    case MBOARD_PROP_TX_DBOARD:
    case MBOARD_PROP_TX_DBOARD_NAMES:
        throw std::runtime_error("Error: trying to set read-only property on usrp2 mboard");

    }
}
