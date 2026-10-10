#pragma once

namespace Bridge
{
	// Patches AIAgent.dll's import table. Call at SKSE PostLoad, once every plugin is loaded.
	bool Install();
}
