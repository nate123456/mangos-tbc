using PlayerbotWeb.Models;

namespace PlayerbotWeb.Services;

public class PlayerbotRepository
{
    public Task<IEnumerable<PlayerbotScript>> GetScriptsAsync(string token)
    {
        var scripts = new List<PlayerbotScript>
            { new() { Name = "test script from token " + token, Script = "yay some lua" } };

        return Task.FromResult(scripts.AsEnumerable());
    }
}
