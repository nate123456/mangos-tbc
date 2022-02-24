using Playerbot.Api.Models;

namespace Playerbot.Api.Abstractions;

public interface IPlayerbotRepository
{
    Task<PlayerbotToken?> GetTokenFromStringAsync(string tokenStr);
    Task<IEnumerable<PlayerbotScript>> GetScriptsAsync(int accountId);
    void SetScripts(int accountId, IEnumerable<PlayerbotScript> scripts, bool isComplete);
    Task DeleteScriptsAsync(int accountId, IEnumerable<string> dtoScriptNames);
}
