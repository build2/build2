// file      : build2/cxx/windows-manifest.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <fstream>

#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/variable>
#include <build2/filesystem>
#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
  {
    // Translate the compiler target CPU value to the processorArchitecture
    // attribute value.
    //
    const char*
    windows_manifest_arch (const string& tcpu)
    {
      const char* pa (tcpu == "i386" || tcpu == "i686"  ? "x86"   :
                      tcpu == "x86_64"                  ? "amd64" :
                      nullptr);

      if (pa == nullptr)
        fail << "unable to translate CPU " << tcpu << " to manifest "
             << "processor architecture";

      return pa;
    }

    // Generate a Windows manifest and if necessary create/update the manifest
    // file corresponding to the exe{} target. Return the manifest file path.
    //
    path
    windows_manifest (file& t, bool rpath_assembly)
    {
      tracer trace ("cxx::windows_manifest");

      scope& rs (t.root_scope ());

      const char* pa (
        windows_manifest_arch (
          cast<string> (rs["cxx.target.cpu"])));

      string m;

      m += "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n";
      m += "<assembly xmlns='urn:schemas-microsoft-com:asm.v1'\n";
      m += "          manifestVersion='1.0'>\n";

      // Program name, version, etc.
      //
      string name (t.path ().leaf ().string ());

      m += "  <assemblyIdentity name='"; m += name; m += "'\n";
      m += "                    type='win32'\n";
      m += "                    processorArchitecture='"; m += pa; m += "'\n";
      m += "                    version='0.0.0.0'/>\n";

      // Our rpath-emulating assembly.
      //
      if (rpath_assembly)
      {
        m += "  <dependency>\n";
        m += "    <dependentAssembly>\n";
        m += "      <assemblyIdentity name='"; m += name; m += ".dlls'\n";
        m += "                        type='win32'\n";
        m += "                        processorArchitecture='"; m += pa; m += "'\n";
        m += "                        language='*'\n";
        m += "                        version='0.0.0.0'/>\n";
        m += "    </dependentAssembly>\n";
        m += "  </dependency>\n";
      }

      // UAC information. Without it Windows will try to guess, which, as you
      // can imagine, doesn't end well.
      //
      m += "  <trustInfo xmlns='urn:schemas-microsoft-com:asm.v3'>\n";
      m += "    <security>\n";
      m += "      <requestedPrivileges>\n";
      m += "        <requestedExecutionLevel level='asInvoker' uiAccess='false'/>\n";
      m += "      </requestedPrivileges>\n";
      m += "    </security>\n";
      m += "  </trustInfo>\n";

      m += "</assembly>\n";

      // If the manifest file exists, compare to its content. If nothing
      // changed (common case), then we can avoid any further updates.
      //
      // The potentially faster alternative would be to hash it and store an
      // entry in depdb. This, however, gets a bit complicated since we will
      // need to avoid a race between the depdb and .manifest updates.
      //
      path mf (t.path () + ".manifest");

      if (file_exists (mf))
      {
        ifstream ifs (mf.string ());
        string s;
        getline (ifs, s, '\0');

        if (s == m)
          return mf;
      }

      if (verb >= 3)
        text << "cat >" << mf;

      try
      {
        ofstream ofs;
        ofs.exceptions (ofstream::failbit | ofstream::badbit);
        ofs.open (mf.string (), ofstream::out | ofstream::trunc);
        ofs << m;
      }
      catch (const ofstream::failure&)
      {
        fail << "unable to write to " << m;
      }

      return mf;
    }
  }
}
