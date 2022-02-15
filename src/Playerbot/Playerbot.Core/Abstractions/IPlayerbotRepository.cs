using Playerbot.Core.Models;

namespace Playerbot.Core.Abstractions;

public interface IPlayerbotRepository
{
    public Task<PlayerbotToken?> GetTokenFromStringAsync(string tokenStr);
    public Task<IEnumerable<PlayerbotScript>> GetScriptsAsync(int accountId);
    public Task SetScriptsAsync(int accountId, IEnumerable<PlayerbotScript> scripts);
}
