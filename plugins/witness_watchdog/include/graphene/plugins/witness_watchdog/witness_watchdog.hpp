#pragma once
#include <appbase/application.hpp>
#include <graphene/plugins/chain/plugin.hpp>

namespace graphene { namespace plugins { namespace witness_watchdog {

using namespace graphene::chain;

class witness_watchdog_plugin : public appbase::plugin<witness_watchdog_plugin> {
public:
   witness_watchdog_plugin();
   virtual ~witness_watchdog_plugin();

   APPBASE_PLUGIN_REQUIRES((graphene::plugins::chain::plugin))

   static const std::string& name() { static std::string name = "witness_watchdog"; return name; }

   virtual void set_program_options(boost::program_options::options_description& cli, boost::program_options::options_description& cfg) override;
   virtual void plugin_initialize(const boost::program_options::variables_map& options) override;
   virtual void plugin_startup() override;
   virtual void plugin_shutdown() override;

private:
   struct impl;
   std::unique_ptr<impl> my;
};

} } } // graphene::plugins::witness_watchdog